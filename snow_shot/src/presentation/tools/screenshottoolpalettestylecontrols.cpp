#include "screenshottoolpalettestylecontrols.h"
#include "screenshottoolbarperfinstrumentation.h"

#include "screenshottoolpalettestylepresets.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"

#include "antd_icons.h"
#include "widgets/color_picker.h"
#include "widgets/input_line_edit.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/radio_button_group.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QWidget>

#include <utility>

#include <algorithm>
#include <iterator>
#include <limits>

namespace {
quint64 propertyGroupCount(quint32 groups, quint32 allGroups) {
    quint64 count = 0;
    for (quint32 remaining = groups & allGroups; remaining != 0; remaining &= remaining - 1) {
        ++count;
    }
    return count;
}

namespace outlined_icons = adqt::icons::antd::outlined;
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace style_presets = snow_shot::presentation::style_presets;
constexpr int kCornerRadiusLeadingSpacing = 4;
constexpr int kTextStrokeColorTrailingSpacing = 4;
constexpr int kSerialNumberTrailingSpacing = 4;
constexpr int kWatermarkTextWidth = 135;
constexpr int kOpacitySliderWidth = 96;
constexpr int kCompactSliderIconSize = 16;
constexpr int kCompactSliderWidth = 96;
constexpr double kMinWatermarkFontSize = 6.0;
constexpr double kMaxWatermarkFontSize = 512.0;

[[maybe_unused]] constexpr const char* kStyleControlTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Rectangle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Ellipse"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Diamond"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Stroke color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Stroke color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Stroke width %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Fill color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Fill color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Fill color transparent"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight stroke width %1px"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight stroke color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen highlight color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen highlight color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Arrow stroke color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Arrow stroke color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Arrow stroke width %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text font size %1 (%2px)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text font family"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Font family"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Default"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Mixed"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text alignment"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text stroke width %1px"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text stroke color transparent"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text stroke color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text fill color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text fill color transparent"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text fill color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark text"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark font family"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number font size %1px"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number font family"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number fill color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number fill color transparent"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number fill color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Mask color"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Mask color %1"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current pen filter stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen filter stroke width %1 (%2px)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Dashed stroke"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Dotted stroke"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Solid stroke"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Cross-line fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Line fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Solid fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pick color from canvas"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current pen highlight stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen highlight stroke width %1 (%2px)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Dashed arrow stroke"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Dotted arrow stroke"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Solid arrow stroke"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current arrow stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current text font size"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Align text left"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Align text center"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Align text right"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Cross-line text fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Line text fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Solid text fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current watermark font size"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark font size %1 (%2px)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark angle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark gap"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Current sequence number font size"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Cross-line sequence number fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Line sequence number fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Solid sequence number fill"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "%1 (unavailable)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight stroke width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Opacity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Adjust opacity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Filter type"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Mosaic"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Gaussian blur"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Grayscale"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Inversion"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Filter intensity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Adjust filter intensity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen filter"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Rectangle filter"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen highlight"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Rectangle highlight"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Corner radius (scroll to adjust)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text fill corner radius (scroll to adjust)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Sequence number (scroll to adjust)"),
};

[[maybe_unused]] constexpr const char* kStartArrowheadOptionTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead none"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead standard"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead bar"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead dot"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead circle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead circle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead triangle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead triangle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead diamond"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead diamond outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead crowfoot one"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead crowfoot many"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead crowfoot one or many"),
};

[[maybe_unused]] constexpr const char* kEndArrowheadOptionTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead none"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead standard"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead bar"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead dot"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead circle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead circle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead triangle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead triangle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead diamond"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead diamond outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead crowfoot one"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead crowfoot many"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead crowfoot one or many"),
};

int defaultCornerRadius() {
    return qRound(
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.cornerRadii.topLeft);
}

SnowCanvasShapeStyle shapeStyleFromArrowStyle(const SnowCanvasArrowStyle& arrowStyle) {
    SnowCanvasShapeStyle style;
    style.stroke = arrowStyle.stroke;
    style.strokeWidth = arrowStyle.strokeWidth;
    style.startArrowhead = arrowStyle.startArrowhead;
    style.endArrowhead = arrowStyle.endArrowhead;
    style.strokeStyle = arrowStyle.strokeStyle;
    style.arrowType = arrowStyle.arrowType;
    return style;
}

SnowCanvasArrowStyle arrowStyleFromShapeStyle(const SnowCanvasShapeStyle& shapeStyle) {
    SnowCanvasArrowStyle style;
    style.stroke = shapeStyle.stroke;
    style.strokeWidth = shapeStyle.strokeWidth;
    style.startArrowhead = shapeStyle.startArrowhead;
    style.endArrowhead = shapeStyle.endArrowhead;
    style.strokeStyle = shapeStyle.strokeStyle;
    style.arrowType = shapeStyle.arrowType;
    return style;
}

adqt::icons::IconRef arrowTypeIcon(SnowCanvasArrowType arrowType) {
    switch (arrowType) {
    case SnowCanvasArrowType::Straight:
        return custom_outlined_icons::ArrowTypeStraight();
    case SnowCanvasArrowType::Curve:
        return custom_outlined_icons::ArrowTypeCurved();
    case SnowCanvasArrowType::Elbow:
        return custom_outlined_icons::ArrowTypeElbow();
    }
    return custom_outlined_icons::ArrowTypeStraight();
}

adqt::icons::IconRef textAlignmentIcon(SnowCanvasTextHorizontalAlign alignment) {
    switch (alignment) {
    case SnowCanvasTextHorizontalAlign::Left:
        return outlined_icons::AlignLeft();
    case SnowCanvasTextHorizontalAlign::Center:
        return outlined_icons::AlignCenter();
    case SnowCanvasTextHorizontalAlign::Right:
        return outlined_icons::AlignRight();
    }
    return outlined_icons::AlignLeft();
}

adqt::icons::IconRef arrowheadIcon(SnowCanvasArrowhead arrowhead) {
    switch (arrowhead) {
    case SnowCanvasArrowhead::None:
        return custom_outlined_icons::ArrowheadNone();
    case SnowCanvasArrowhead::Arrow:
        return custom_outlined_icons::ArrowheadStandard();
    case SnowCanvasArrowhead::Bar:
        return custom_outlined_icons::ArrowheadBar();
    case SnowCanvasArrowhead::Dot:
        return custom_outlined_icons::ArrowheadDot();
    case SnowCanvasArrowhead::Circle:
        return custom_outlined_icons::ArrowheadCircle();
    case SnowCanvasArrowhead::CircleOutline:
        return custom_outlined_icons::ArrowheadCircleOutline();
    case SnowCanvasArrowhead::Triangle:
        return custom_outlined_icons::ArrowheadTriangle();
    case SnowCanvasArrowhead::TriangleOutline:
        return custom_outlined_icons::ArrowheadTriangleOutline();
    case SnowCanvasArrowhead::Diamond:
        return custom_outlined_icons::ArrowheadDiamond();
    case SnowCanvasArrowhead::DiamondOutline:
        return custom_outlined_icons::ArrowheadDiamondOutline();
    case SnowCanvasArrowhead::CrowfootOne:
        return custom_outlined_icons::ArrowheadCrowfootOne();
    case SnowCanvasArrowhead::CrowfootMany:
        return custom_outlined_icons::ArrowheadCrowfootMany();
    case SnowCanvasArrowhead::CrowfootOneOrMany:
        return custom_outlined_icons::ArrowheadCrowfootOneOrMany();
    }
    return custom_outlined_icons::ArrowheadNone();
}

const char* arrowheadOptionTooltipSource(bool start, SnowCanvasArrowhead arrowhead) {
    const auto* options =
        start ? kStartArrowheadOptionTranslations : kEndArrowheadOptionTranslations;
    const int index = static_cast<int>(arrowhead);
    const int optionCount = start ? static_cast<int>(std::size(kStartArrowheadOptionTranslations))
                                  : static_cast<int>(std::size(kEndArrowheadOptionTranslations));
    if (index >= 0 && index < optionCount) {
        return options[index];
    }
    return options[0];
}
} // namespace

ScreenshotToolPaletteStyleControls::ScreenshotToolPaletteStyleControls(
    ScreenshotToolPaletteStyleControlCallbacks callbacks, const SnowCanvasStyleDefaults& defaults)
    : m_state(defaults), m_callbacks(std::move(callbacks)), m_defaults(defaults) {}

ScreenshotToolPaletteStyleState& ScreenshotToolPaletteStyleControls::styleState() {
    return m_state;
}

const ScreenshotToolPaletteStyleState& ScreenshotToolPaletteStyleControls::styleState() const {
    return m_state;
}

QWidget* ScreenshotToolPaletteStyleControls::createRowWidget(
    QWidget* panel, const QString& objectName,
    const ScreenshotToolPaletteStyleFamilyHost& host) const {
    auto* controls = new QWidget(panel);
    controls->setObjectName(objectName);
    auto* layout = new QHBoxLayout(controls);
    if (host.registerRowLayout) {
        host.registerRowLayout(layout);
    }
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(host.rowItemSpacing);
    return controls;
}

void ScreenshotToolPaletteStyleControls::registerEditor(
    ScreenshotToolPaletteStyleEditorComponent* component) {
    if (component != nullptr) {
        m_registeredComponents.push_back(component);
    }
}

void ScreenshotToolPaletteStyleControls::addToolbarSpacing(
    QBoxLayout* layout, int baseSpacing, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr) {
        return;
    }

    auto* spacer = new QSpacerItem(qMax(1, qRound(baseSpacing * metrics.physicalScale)), 0,
                                   QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout->addSpacerItem(spacer);
    m_toolbarSpacingItems.push_back(
        ToolbarSpacingItem{spacer, layout->parentWidget(), baseSpacing});
}

ScreenshotToolPaletteShapeFamilyResult ScreenshotToolPaletteStyleControls::buildShapeFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.shape");
    ScreenshotToolPaletteShapeFamilyResult result;
    if (panel == nullptr) {
        return result;
    }
    QWidget* controls =
        createRowWidget(panel, QStringLiteral("screenshotRectangleStyleControls"), host);
    auto* layout = static_cast<QHBoxLayout*>(controls->layout());

    ScreenshotToolPaletteRadioEditorConfig shapeConfig;
    shapeConfig.objectName = QStringLiteral("screenshotShapeButtonGroup");
    shapeConfig.options = {
        {0, QStringLiteral("Rectangle"), custom_outlined_icons::ShapeRectangle()},
        {1, QStringLiteral("Ellipse"), custom_outlined_icons::ShapeEllipse()},
        {2, QStringLiteral("Diamond"), custom_outlined_icons::ShapeDiamond()},
    };
    shapeConfig.initialId = 0;
    const ScreenshotToolPaletteRadioEditor shapeEditor =
        createScreenshotToolPaletteRadioEditor(controls, shapeConfig, metrics);
    m_shapeControlsContainer = shapeEditor.container;
    m_shapeButtonGroup = shapeEditor.group;
    QObject::connect(m_shapeButtonGroup, &adqt::widgets::AdRadioButtonGroup::checkedIdChanged,
                     controls, [this](int id) {
                         setShape(id == 1   ? SnowCanvasRectangleShape::Ellipse
                                  : id == 2 ? SnowCanvasRectangleShape::Diamond
                                            : SnowCanvasRectangleShape::Rectangle);
                     });
    layout->addWidget(m_shapeControlsContainer);

    if (host.addGroupSpacing) {
        result.shapeGroupSeparatorLeadingSpacing = host.addGroupSpacing(layout);
    }
    if (host.createSeparator) {
        result.shapeGroupSeparator =
            host.createSeparator(controls, QStringLiteral("screenshotShapeStyleGroupSeparator"));
        layout->addWidget(result.shapeGroupSeparator);
    }
    if (host.addGroupSpacing) {
        result.shapeGroupSeparatorTrailingSpacing = host.addGroupSpacing(layout);
    }

    ScreenshotToolPaletteStrokeEditorConfig strokeConfig;
    strokeConfig.accessibleName = QStringLiteral("Stroke color");
    strokeConfig.popupObjectName = QStringLiteral("screenshotStrokeOptions");
    strokeConfig.styleRowObjectName = QStringLiteral("screenshotStrokeStyles");
    strokeConfig.colorValues = m_state.m_rectangleStyle.strokeColorValues();
    strokeConfig.colorTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Stroke color %1").arg(color.name());
    };
    strokeConfig.styleTooltip = [](SnowCanvasStrokeStyle style) {
        return style == SnowCanvasStrokeStyle::Dashed
                   ? ScreenshotToolPaletteTranslationText("Dashed stroke")
               : style == SnowCanvasStrokeStyle::Dotted
                   ? ScreenshotToolPaletteTranslationText("Dotted stroke")
                   : ScreenshotToolPaletteTranslationText("Solid stroke");
    };
    m_shapeStrokeEditor = std::make_unique<ScreenshotToolPaletteStrokeEditor>();
    registerEditor(m_shapeStrokeEditor.get());
    m_shapeStrokeEditor->build(
        layout, controls, controls, strokeConfig, m_state.m_rectangleStyle.strokeColor(),
        m_state.m_rectangleStyle.strokeStyle(),
        [this](const QColor& color) { setStrokeColor(color); },
        [this](SnowCanvasStrokeStyle style) { setStrokeStyle(style); }, editorServices(), metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    ScreenshotToolPaletteNumericPresetEditorConfig strokeWidthConfig;
    strokeWidthConfig.summaryTooltip = QStringLiteral("Current stroke width");
    strokeWidthConfig.values = m_state.m_rectangleStyle.strokeWidthValues();
    strokeWidthConfig.strokePreview = true;
    strokeWidthConfig.presetTooltip = [](int, double value) {
        return ScreenshotToolPaletteTranslationText("Stroke width %1").arg(value, 0, 'g', 2);
    };
    m_shapeStrokeWidthEditor = std::make_unique<ScreenshotToolPaletteNumericPresetEditor>();
    registerEditor(m_shapeStrokeWidthEditor.get());
    m_shapeStrokeWidthEditor->build(
        layout, controls, controls, strokeWidthConfig, m_state.m_rectangleStyle.strokeWidth(),
        [this]() { cycleStrokeWidth(); }, [this](double value) { setStrokeWidth(value); }, metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    ScreenshotToolPaletteFillEditorConfig fillConfig;
    fillConfig.accessibleName = QStringLiteral("Fill color");
    fillConfig.popupObjectName = QStringLiteral("screenshotFillOptions");
    fillConfig.presetRowObjectName = QStringLiteral("screenshotFillColorPresets");
    fillConfig.colorValues = m_state.m_rectangleStyle.fillColorValues();
    fillConfig.colorTooltip = [](const QColor& color) {
        return color.alpha() == 0
                   ? ScreenshotToolPaletteTranslationText("Fill color transparent")
                   : ScreenshotToolPaletteTranslationText("Fill color %1").arg(color.name());
    };
    fillConfig.styleTooltip = [](SnowCanvasFillStyle style) {
        return style == SnowCanvasFillStyle::CrossLine
                   ? ScreenshotToolPaletteTranslationText("Cross-line fill")
               : style == SnowCanvasFillStyle::Line
                   ? ScreenshotToolPaletteTranslationText("Line fill")
                   : ScreenshotToolPaletteTranslationText("Solid fill");
    };
    m_shapeFillEditor = std::make_unique<ScreenshotToolPaletteFillEditor>();
    registerEditor(m_shapeFillEditor.get());
    m_shapeFillEditor->build(
        layout, controls, controls, fillConfig, m_state.m_rectangleStyle.fillColor(),
        m_state.m_rectangleStyle.fillStyle(), [this](const QColor& color) { setFillColor(color); },
        [this](SnowCanvasFillStyle style) { setFillStyle(style); }, editorServices(), metrics);

    if (host.addItemSpacing) {
        host.addItemSpacing(layout);
    }

    m_cornerRadiusEditor = createScreenshotToolPaletteCornerRadiusEditor(
        controls, "Corner radius (scroll to adjust)", custom_outlined_icons::SelectionRadius(),
        m_state.m_rectangleStyle.cornerRadius(), metrics);
    m_cornerRadiusEditor->setObjectName(QStringLiteral("screenshotSelectionCornerRadiusButton"));
    QObject::connect(m_cornerRadiusEditor, &adqt::widgets::AdButton::clicked, controls,
                     [this]() { setCornerRadius(defaultCornerRadius()); });
    layout->addWidget(m_cornerRadiusEditor);

    registerShapeEntries();
    result.controls = controls;
    return result;
}

QWidget* ScreenshotToolPaletteStyleControls::buildArrowFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.arrow");
    if (panel == nullptr) {
        return nullptr;
    }
    QWidget* controls =
        createRowWidget(panel, QStringLiteral("screenshotArrowStyleControls"), host);
    auto* layout = static_cast<QHBoxLayout*>(controls->layout());

    ScreenshotToolPaletteStrokeEditorConfig arrowStrokeConfig;
    arrowStrokeConfig.accessibleName = QStringLiteral("Arrow stroke color");
    arrowStrokeConfig.colorValues = style_presets::strokeColors();
    arrowStrokeConfig.colorTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Arrow stroke color %1").arg(color.name());
    };
    arrowStrokeConfig.styleTooltip = [](SnowCanvasStrokeStyle style) {
        return style == SnowCanvasStrokeStyle::Dashed
                   ? ScreenshotToolPaletteTranslationText("Dashed arrow stroke")
               : style == SnowCanvasStrokeStyle::Dotted
                   ? ScreenshotToolPaletteTranslationText("Dotted arrow stroke")
                   : ScreenshotToolPaletteTranslationText("Solid arrow stroke");
    };
    m_arrowStrokeEditor = std::make_unique<ScreenshotToolPaletteStrokeEditor>();
    registerEditor(m_arrowStrokeEditor.get());
    m_arrowStrokeEditor->build(
        layout, controls, controls, arrowStrokeConfig, m_state.m_arrowStyle.stroke,
        m_state.m_arrowStyle.strokeStyle,
        [this](const QColor& color) { setArrowStrokeColor(color); },
        [this](SnowCanvasStrokeStyle style) { setArrowStrokeStyle(style); }, editorServices(),
        metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    ScreenshotToolPaletteNumericPresetEditorConfig arrowStrokeWidthConfig;
    arrowStrokeWidthConfig.summaryTooltip = QStringLiteral("Current arrow stroke width");
    arrowStrokeWidthConfig.values = style_presets::strokePresetWidths();
    arrowStrokeWidthConfig.strokePreview = true;
    arrowStrokeWidthConfig.presetTooltip = [](int, double value) {
        return ScreenshotToolPaletteTranslationText("Arrow stroke width %1").arg(value, 0, 'g', 2);
    };
    m_arrowStrokeWidthEditor = std::make_unique<ScreenshotToolPaletteNumericPresetEditor>();
    registerEditor(m_arrowStrokeWidthEditor.get());
    m_arrowStrokeWidthEditor->build(
        layout, controls, controls, arrowStrokeWidthConfig, m_state.m_arrowStyle.strokeWidth,
        [this]() { cycleArrowStrokeWidth(); }, [this](double value) { setArrowStrokeWidth(value); },
        metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    ScreenshotToolPaletteRadioEditorConfig arrowTypeConfig;
    arrowTypeConfig.objectName = QStringLiteral("screenshotArrowTypeButtonGroup");
    arrowTypeConfig.options = {
        {0, QStringLiteral("Straight arrow"), arrowTypeIcon(SnowCanvasArrowType::Straight)},
        {1, QStringLiteral("Curved arrow"), arrowTypeIcon(SnowCanvasArrowType::Curve)},
        {2, QStringLiteral("Elbow arrow"), arrowTypeIcon(SnowCanvasArrowType::Elbow)},
    };
    arrowTypeConfig.initialId = 0;
    const ScreenshotToolPaletteRadioEditor arrowTypeEditor =
        createScreenshotToolPaletteRadioEditor(controls, arrowTypeConfig, metrics);
    QWidget* arrowTypeControlsContainer = arrowTypeEditor.container;
    m_arrowTypeButtonGroup = arrowTypeEditor.group;
    QObject::connect(m_arrowTypeButtonGroup, &adqt::widgets::AdRadioButtonGroup::checkedIdChanged,
                     controls, [this](int id) {
                         setArrowType(id == 1   ? SnowCanvasArrowType::Curve
                                      : id == 2 ? SnowCanvasArrowType::Elbow
                                                : SnowCanvasArrowType::Straight);
                     });
    layout->addWidget(arrowTypeControlsContainer);

    const QVector<SnowCanvasArrowhead> arrowheads{
        SnowCanvasArrowhead::None,
        SnowCanvasArrowhead::Arrow,
        SnowCanvasArrowhead::Bar,
        SnowCanvasArrowhead::Dot,
        SnowCanvasArrowhead::Circle,
        SnowCanvasArrowhead::CircleOutline,
        SnowCanvasArrowhead::Triangle,
        SnowCanvasArrowhead::TriangleOutline,
        SnowCanvasArrowhead::Diamond,
        SnowCanvasArrowhead::DiamondOutline,
        SnowCanvasArrowhead::CrowfootOne,
        SnowCanvasArrowhead::CrowfootMany,
        SnowCanvasArrowhead::CrowfootOneOrMany,
    };
    const auto addArrowheadEditor = [this, layout, controls, metrics,
                                     &arrowheads](bool start, const QString& accessibleName) {
        ScreenshotToolPaletteIconOptionEditorConfig config;
        config.accessibleName = accessibleName;
        config.triggerTooltip = accessibleName;
        config.gridColumnCount = 4;
        for (SnowCanvasArrowhead arrowhead : arrowheads) {
            config.options.push_back({
                static_cast<int>(arrowhead),
                arrowheadOptionTooltipSource(start, arrowhead),
                arrowheadIcon(arrowhead),
            });
        }
        const SnowCanvasArrowhead currentArrowhead =
            start ? m_state.m_arrowStyle.startArrowhead : m_state.m_arrowStyle.endArrowhead;
        auto editor = std::make_unique<ScreenshotToolPaletteIconOptionEditor>();
        editor->build(
            layout, controls, controls, config, static_cast<int>(currentArrowhead),
            [this, start](int value) {
                setArrowhead(start, static_cast<SnowCanvasArrowhead>(value));
            },
            metrics);
        registerEditor(editor.get());
        return editor;
    };
    m_startArrowheadEditor = addArrowheadEditor(true, QStringLiteral("Start arrowhead"));
    m_endArrowheadEditor = addArrowheadEditor(false, QStringLiteral("End arrowhead"));

    registerArrowEntries();
    updateArrowStyleControls();
    return controls;
}

ScreenshotToolPaletteHighlightFamilyResult ScreenshotToolPaletteStyleControls::buildHighlightFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.highlight");
    ScreenshotToolPaletteHighlightFamilyResult result;
    if (panel == nullptr) {
        return result;
    }

    QWidget* rectangleControls =
        createRowWidget(panel, QStringLiteral("screenshotHighlightStyleControls"), host);
    auto* rectangleLayout = static_cast<QHBoxLayout*>(rectangleControls->layout());

    ScreenshotToolPaletteColorEditorConfig highlightColorConfig;
    highlightColorConfig.accessibleName = QStringLiteral("Highlight color");
    highlightColorConfig.presetValues = m_state.m_textStyle.colorValues();
    highlightColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Highlight color %1").arg(color.name());
    };
    m_highlightColorEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
    registerEditor(m_highlightColorEditor.get());
    m_highlightColorEditor->build(
        rectangleLayout, rectangleControls, rectangleControls, highlightColorConfig,
        m_state.m_highlightStyle.fillColor(), [this](const QColor& color) { setFillColor(color); },
        {}, editorServices(), metrics);
    if (host.addGroupSeparator) {
        host.addGroupSeparator(rectangleLayout);
    }

    ScreenshotToolPaletteWidthColorEditorConfig highlightStrokeConfig;
    highlightStrokeConfig.accessibleName = QStringLiteral("Highlight stroke width");
    highlightStrokeConfig.triggerTooltip = QStringLiteral("Highlight stroke width");
    highlightStrokeConfig.popupObjectName = QStringLiteral("screenshotHighlightStrokeOptions");
    highlightStrokeConfig.widthRowObjectName =
        QStringLiteral("screenshotHighlightStrokeWidthPresets");
    highlightStrokeConfig.colorRowObjectName =
        QStringLiteral("screenshotHighlightStrokeColorPresets");
    highlightStrokeConfig.widthValues = m_state.m_highlightStyle.strokeWidthValues();
    highlightStrokeConfig.colorValues = m_state.m_highlightStyle.strokeColorValues();
    highlightStrokeConfig.widthTooltip = [](double width) {
        return ScreenshotToolPaletteTranslationText("Highlight stroke width %1px")
            .arg(width, 0, 'g', 3);
    };
    highlightStrokeConfig.colorTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Highlight stroke color %1").arg(color.name());
    };
    m_highlightStrokeEditor = std::make_unique<ScreenshotToolPaletteWidthColorEditor>();
    registerEditor(m_highlightStrokeEditor.get());
    m_highlightStrokeEditor->build(
        rectangleLayout, rectangleControls, rectangleControls, highlightStrokeConfig,
        m_state.m_highlightStyle.strokeWidth(), m_state.m_highlightStyle.strokeColor(),
        [this](double width) { setStrokeWidth(width); },
        [this](const QColor& color) { setStrokeColor(color); }, editorServices(), metrics);

    const QVector<ScreenshotToolPaletteStyleModeSelectorOption> modes{
        {static_cast<int>(ScreenshotToolPalette::Tool::PenHighlight),
         QStringLiteral("Pen highlight"), outlined_icons::Highlight()},
        {static_cast<int>(ScreenshotToolPalette::Tool::RectangleHighlight),
         QStringLiteral("Rectangle highlight"), custom_outlined_icons::ShapeRectangle()},
    };
    if (host.createModeSelector) {
        rectangleLayout->insertWidget(
            0, host.createModeSelector(
                   rectangleControls, QStringLiteral("screenshotHighlightModeSelector"),
                   static_cast<int>(ScreenshotToolPalette::Tool::PenHighlight), modes));
    }
    if (host.insertGroupSpacing) {
        host.insertGroupSpacing(rectangleLayout, 1);
    }
    registerHighlightEntries();
    result.rectangleControls = rectangleControls;

    QWidget* penControls =
        createRowWidget(panel, QStringLiteral("screenshotPenHighlightStyleControls"), host);
    auto* penLayout = static_cast<QHBoxLayout*>(penControls->layout());

    ScreenshotToolPaletteColorEditorConfig penHighlightColorConfig;
    penHighlightColorConfig.accessibleName = QStringLiteral("Pen highlight color");
    penHighlightColorConfig.presetValues = m_state.m_textStyle.colorValues();
    penHighlightColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Pen highlight color %1").arg(color.name());
    };
    m_penHighlightColorEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
    registerEditor(m_penHighlightColorEditor.get());
    m_penHighlightColorEditor->build(
        penLayout, penControls, penControls, penHighlightColorConfig,
        m_state.m_penHighlightStyle.stroke,
        [this](const QColor& color) { setPenHighlightColor(color); }, {}, editorServices(),
        metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(penLayout);
    }
    ScreenshotToolPaletteNumericPresetEditorConfig penHighlightWidthConfig =
        snow_shot::presentation::screenshotToolPaletteSizePresetEditorConfig(
            QStringLiteral("Current pen highlight stroke width"), QString(),
            "Pen highlight stroke width %1 (%2px)");
    m_penHighlightStrokeWidthEditor = std::make_unique<ScreenshotToolPaletteNumericPresetEditor>();
    registerEditor(m_penHighlightStrokeWidthEditor.get());
    m_penHighlightStrokeWidthEditor->build(
        penLayout, penControls, penControls, penHighlightWidthConfig,
        m_state.m_penHighlightStyle.strokeWidth, {},
        [this](double value) { setPenHighlightStrokeWidth(value); }, metrics);

    if (host.createModeSelector) {
        penLayout->insertWidget(
            0, host.createModeSelector(
                   penControls, QStringLiteral("screenshotHighlightModeSelector"),
                   static_cast<int>(ScreenshotToolPalette::Tool::PenHighlight), modes));
    }
    if (host.insertGroupSpacing) {
        host.insertGroupSpacing(penLayout, 1);
    }
    registerPenHighlightEntries();
    updatePenHighlightStyleControls();
    result.penControls = penControls;
    return result;
}

QWidget* ScreenshotToolPaletteStyleControls::buildSpotlightFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteSpotlightCallbacks& spotlightCallbacks,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.spotlight");
    if (panel == nullptr) {
        return nullptr;
    }
    QWidget* controls =
        createRowWidget(panel, QStringLiteral("screenshotSpotlightStyleControls"), host);
    auto* layout = static_cast<QHBoxLayout*>(controls->layout());

    ScreenshotToolPaletteColorEditorConfig spotlightColorConfig;
    spotlightColorConfig.accessibleName = QStringLiteral("Mask color");
    spotlightColorConfig.pickerObjectName = QStringLiteral("screenshotSpotlightColorPicker");
    spotlightColorConfig.triggerObjectName = QStringLiteral("screenshotSpotlightColorTrigger");
    spotlightColorConfig.presetValues = style_presets::textColors();
    spotlightColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Mask color %1").arg(color.name());
    };
    m_spotlightColorEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
    registerEditor(m_spotlightColorEditor.get());
    m_spotlightColorEditor->build(layout, controls, controls, spotlightColorConfig,
                                  m_state.spotlightConfig.color, spotlightCallbacks.commitColor,
                                  spotlightCallbacks.previewColor, editorServices(), metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    ScreenshotToolPaletteSliderEditorConfig opacityConfig;
    opacityConfig.iconObjectName = QStringLiteral("screenshotSpotlightOpacityIcon");
    opacityConfig.sliderObjectName = QStringLiteral("screenshotSpotlightOpacitySlider");
    opacityConfig.accessibleName = QStringLiteral("Opacity");
    opacityConfig.sliderTooltip = QStringLiteral("Adjust opacity");
    opacityConfig.iconRef = custom_outlined_icons::Opacity();
    opacityConfig.initialValue =
        qRound(std::clamp(m_state.spotlightConfig.opacity, 0.0, 1.0) * 100.0);
    opacityConfig.baseIconSize = kCompactSliderIconSize;
    opacityConfig.baseSliderWidth = kCompactSliderWidth;
    m_spotlightOpacityEditor =
        createScreenshotToolPaletteSliderEditor(layout, controls, opacityConfig, metrics);
    QObject::connect(m_spotlightOpacityEditor.slider, &adqt::widgets::AdSlider::valueChanged,
                     controls, [this, spotlightCallbacks](double value) {
                         m_spotlightOpacityEditor.slider->setAccessibleDescription(
                             QStringLiteral("%1%").arg(qRound(value)));
                         if (spotlightCallbacks.setOpacity) {
                             spotlightCallbacks.setOpacity(std::clamp(value / 100.0, 0.0, 1.0));
                         }
                     });
    return controls;
}

QWidget* ScreenshotToolPaletteStyleControls::buildTextFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.text");
    if (panel == nullptr) {
        return nullptr;
    }
    QWidget* controls = createRowWidget(panel, QStringLiteral("screenshotTextStyleControls"), host);
    auto* layout = static_cast<QHBoxLayout*>(controls->layout());
    const SnowCanvasTextStyle& style = m_state.m_textStyle.textStyle();

    ScreenshotToolPaletteColorEditorConfig textColorConfig;
    textColorConfig.accessibleName = QStringLiteral("Text color");
    textColorConfig.presetValues = m_state.m_textStyle.colorValues();
    textColorConfig.observePopup = true;
    textColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Text color %1").arg(color.name());
    };
    m_textColorEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
    registerEditor(m_textColorEditor.get());
    m_textColorEditor->build(
        layout, controls, controls, textColorConfig, style.color,
        [this](const QColor& color) { setTextColor(color); }, {}, editorServices(), metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }
    ScreenshotToolPaletteFontEditorConfig textFontConfig;
    textFontConfig.accessibleName = QStringLiteral("Text font family");
    textFontConfig.summaryTooltip = QStringLiteral("Current text font size");
    textFontConfig.sizeValues = m_state.m_textStyle.fontSizeValues();
    textFontConfig.observePopup = true;
    textFontConfig.presetTooltip = [](int index, double value) {
        return style_presets::sizePresetTooltip("Text font size %1 (%2px)", index, value);
    };
    m_textFontEditor = std::make_unique<ScreenshotToolPaletteFontEditor>();
    registerEditor(m_textFontEditor.get());
    m_textFontEditor->build(
        layout, controls, controls, textFontConfig, style.fontSize, style.fontFamily,
        [this]() { cycleTextFontSize(); }, [this](double value) { setTextFontSize(value); },
        [this](const QString& value) { setTextFontFamily(value); }, editorServices(), metrics);

    ScreenshotToolPaletteIconOptionEditorConfig alignmentConfig;
    alignmentConfig.accessibleName = QStringLiteral("Text alignment");
    alignmentConfig.triggerTooltip = QStringLiteral("Text alignment");
    alignmentConfig.minimizeTitleWidth = true;
    const struct AlignmentOption {
        SnowCanvasTextHorizontalAlign value;
        ScreenshotToolPaletteTranslationText tooltip;
    } alignmentOptions[] = {
        {SnowCanvasTextHorizontalAlign::Left, QStringLiteral("Align text left")},
        {SnowCanvasTextHorizontalAlign::Center, QStringLiteral("Align text center")},
        {SnowCanvasTextHorizontalAlign::Right, QStringLiteral("Align text right")},
    };
    for (const AlignmentOption& option : alignmentOptions) {
        alignmentConfig.options.push_back({
            static_cast<int>(option.value),
            option.tooltip,
            textAlignmentIcon(option.value),
        });
    }
    m_textAlignmentEditor = std::make_unique<ScreenshotToolPaletteIconOptionEditor>();
    registerEditor(m_textAlignmentEditor.get());
    m_textAlignmentEditor->build(
        layout, controls, controls, alignmentConfig, static_cast<int>(style.horizontalAlign),
        [this](int value) {
            setTextHorizontalAlign(static_cast<SnowCanvasTextHorizontalAlign>(value));
        },
        metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }
    ScreenshotToolPaletteWidthColorEditorConfig textStrokeConfig;
    textStrokeConfig.accessibleName = QStringLiteral("Text stroke width");
    textStrokeConfig.triggerTooltip = QStringLiteral("Text stroke width");
    textStrokeConfig.popupObjectName = QStringLiteral("screenshotTextStrokeWidthOptions");
    textStrokeConfig.widthRowObjectName = QStringLiteral("screenshotTextStrokeWidthPresets");
    textStrokeConfig.colorRowObjectName = QStringLiteral("screenshotTextStrokeColorPresets");
    textStrokeConfig.widthValues = m_state.m_textStyle.strokeWidthValues();
    textStrokeConfig.colorValues = m_state.m_textStyle.fillColorValues();
    textStrokeConfig.observePopup = true;
    textStrokeConfig.widthTooltip = [](double width) {
        return ScreenshotToolPaletteTranslationText("Text stroke width %1px").arg(width, 0, 'g', 3);
    };
    textStrokeConfig.colorTooltip = [](const QColor& color) {
        return color.alpha() == 0
                   ? ScreenshotToolPaletteTranslationText("Text stroke color transparent")
                   : ScreenshotToolPaletteTranslationText("Text stroke color %1").arg(color.name());
    };
    m_textStrokeEditor = std::make_unique<ScreenshotToolPaletteWidthColorEditor>();
    registerEditor(m_textStrokeEditor.get());
    m_textStrokeEditor->build(
        layout, controls, controls, textStrokeConfig, style.strokeWidth, style.stroke,
        [this](double width) { setTextStrokeWidth(width); },
        [this](const QColor& color) { setTextStrokeColor(color); }, editorServices(), metrics);
    addToolbarSpacing(layout, kTextStrokeColorTrailingSpacing, metrics);

    ScreenshotToolPaletteFillEditorConfig textFillConfig;
    textFillConfig.accessibleName = QStringLiteral("Text fill color");
    textFillConfig.popupObjectName = QStringLiteral("screenshotTextFillOptions");
    textFillConfig.presetRowObjectName = QStringLiteral("screenshotTextFillColorPresets");
    textFillConfig.colorValues = m_state.m_textStyle.fillColorValues();
    textFillConfig.observePopup = true;
    textFillConfig.colorTooltip = [](const QColor& color) {
        return color.alpha() == 0
                   ? ScreenshotToolPaletteTranslationText("Text fill color transparent")
                   : ScreenshotToolPaletteTranslationText("Text fill color %1").arg(color.name());
    };
    textFillConfig.styleTooltip = [](SnowCanvasFillStyle style) {
        return style == SnowCanvasFillStyle::CrossLine
                   ? ScreenshotToolPaletteTranslationText("Cross-line text fill")
               : style == SnowCanvasFillStyle::Line
                   ? ScreenshotToolPaletteTranslationText("Line text fill")
                   : ScreenshotToolPaletteTranslationText("Solid text fill");
    };
    m_textFillEditor = std::make_unique<ScreenshotToolPaletteFillEditor>();
    registerEditor(m_textFillEditor.get());
    m_textFillEditor->build(
        layout, controls, controls, textFillConfig, style.fill, style.fillStyle,
        [this](const QColor& color) { setTextFillColor(color); },
        [this](SnowCanvasFillStyle fillStyle) { setTextFillStyle(fillStyle); }, editorServices(),
        metrics);
    addToolbarSpacing(layout, kCornerRadiusLeadingSpacing, metrics);
    m_textCornerRadiusEditor = createScreenshotToolPaletteCornerRadiusEditor(
        controls, "Text fill corner radius (scroll to adjust)",
        custom_outlined_icons::SelectionRadius(), qRound(style.cornerRadii.topLeft), metrics);
    QObject::connect(m_textCornerRadiusEditor, &adqt::widgets::AdButton::clicked, controls,
                     [this]() { setTextCornerRadius(defaultCornerRadius()); });
    layout->addWidget(m_textCornerRadiusEditor);

    registerTextEntries();
    updateTextStyleControls();
    return controls;
}

QWidget* ScreenshotToolPaletteStyleControls::buildSerialNumberFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.serial_number");
    if (panel == nullptr) {
        return nullptr;
    }
    QWidget* controls =
        createRowWidget(panel, QStringLiteral("screenshotSerialNumberStyleControls"), host);
    auto* layout = static_cast<QHBoxLayout*>(controls->layout());

    ScreenshotToolPaletteColorEditorConfig serialNumberColorConfig;
    serialNumberColorConfig.accessibleName = QStringLiteral("Sequence number color");
    serialNumberColorConfig.presetValues = m_state.m_textStyle.colorValues();
    serialNumberColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Sequence number color %1").arg(color.name());
    };
    m_serialNumberColorEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
    registerEditor(m_serialNumberColorEditor.get());
    m_serialNumberColorEditor->build(
        layout, controls, controls, serialNumberColorConfig, m_state.m_serialNumberStyle.color,
        [this](const QColor& color) { setSerialNumberColor(color); }, {}, editorServices(),
        metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }
    m_serialNumberEditor = createScreenshotToolPaletteCornerRadiusEditor(
        controls, "Sequence number (scroll to adjust)", outlined_icons::Number(),
        static_cast<int>(std::clamp<qint64>(m_state.m_serialNumberStyle.number, 0,
                                            std::numeric_limits<int>::max())),
        metrics);
    layout->addWidget(m_serialNumberEditor);
    addToolbarSpacing(layout, kSerialNumberTrailingSpacing, metrics);

    ScreenshotToolPaletteFontEditorConfig serialNumberFontConfig;
    serialNumberFontConfig.accessibleName = QStringLiteral("Sequence number font family");
    serialNumberFontConfig.summaryTooltip = QStringLiteral("Current sequence number font size");
    serialNumberFontConfig.sizeValues = m_state.m_textStyle.fontSizeValues();
    serialNumberFontConfig.presetTooltip = [](int, double value) {
        return ScreenshotToolPaletteTranslationText("Sequence number font size %1px")
            .arg(value, 0, 'g', 3);
    };
    m_serialNumberFontEditor = std::make_unique<ScreenshotToolPaletteFontEditor>();
    registerEditor(m_serialNumberFontEditor.get());
    m_serialNumberFontEditor->build(
        layout, controls, controls, serialNumberFontConfig, m_state.m_serialNumberStyle.fontSize,
        m_state.m_serialNumberStyle.fontFamily, [this]() { cycleSerialNumberFontSize(); },
        [this](double value) { setSerialNumberFontSize(value); },
        [this](const QString& value) { setSerialNumberFontFamily(value); }, editorServices(),
        metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }
    ScreenshotToolPaletteFillEditorConfig serialNumberFillConfig;
    serialNumberFillConfig.accessibleName = QStringLiteral("Sequence number fill color");
    serialNumberFillConfig.popupObjectName = QStringLiteral("screenshotSerialNumberFillOptions");
    serialNumberFillConfig.presetRowObjectName =
        QStringLiteral("screenshotSerialNumberFillColorPresets");
    serialNumberFillConfig.colorValues = m_state.m_textStyle.fillColorValues();
    serialNumberFillConfig.colorTooltip = [](const QColor& color) {
        return color.alpha() == 0
                   ? ScreenshotToolPaletteTranslationText("Sequence number fill color transparent")
                   : ScreenshotToolPaletteTranslationText("Sequence number fill color %1")
                         .arg(color.name());
    };
    serialNumberFillConfig.styleTooltip = [](SnowCanvasFillStyle style) {
        return style == SnowCanvasFillStyle::CrossLine
                   ? ScreenshotToolPaletteTranslationText("Cross-line sequence number fill")
               : style == SnowCanvasFillStyle::Line
                   ? ScreenshotToolPaletteTranslationText("Line sequence number fill")
                   : ScreenshotToolPaletteTranslationText("Solid sequence number fill");
    };
    m_serialNumberFillEditor = std::make_unique<ScreenshotToolPaletteFillEditor>();
    registerEditor(m_serialNumberFillEditor.get());
    m_serialNumberFillEditor->build(
        layout, controls, controls, serialNumberFillConfig, m_state.m_serialNumberStyle.fill,
        m_state.m_serialNumberStyle.fillStyle,
        [this](const QColor& color) { setSerialNumberFillColor(color); },
        [this](SnowCanvasFillStyle fillStyle) { setSerialNumberFillStyle(fillStyle); },
        editorServices(), metrics);

    registerSerialNumberEntries();
    updateSerialNumberStyleControls();
    return controls;
}

QWidget* ScreenshotToolPaletteStyleControls::buildWatermarkFamily(
    QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.create_style_family.watermark");
    if (panel == nullptr) {
        return nullptr;
    }
    QWidget* controls =
        createRowWidget(panel, QStringLiteral("screenshotWatermarkStyleControls"), host);
    auto* layout = static_cast<QHBoxLayout*>(controls->layout());
    const SnowCanvasWatermarkConfig& config = m_state.m_watermarkConfig;

    ScreenshotToolPaletteColorEditorConfig watermarkColorConfig;
    watermarkColorConfig.accessibleName = QStringLiteral("Watermark color");
    watermarkColorConfig.pickerObjectName = QStringLiteral("screenshotWatermarkColorPicker");
    watermarkColorConfig.triggerObjectName = QStringLiteral("screenshotWatermarkColorTrigger");
    watermarkColorConfig.presetValues = m_state.m_textStyle.colorValues();
    watermarkColorConfig.observePopup = true;
    watermarkColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Watermark color %1").arg(color.name());
    };
    m_watermarkColorEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
    registerEditor(m_watermarkColorEditor.get());
    m_watermarkColorEditor->build(
        layout, controls, controls, watermarkColorConfig, config.color,
        [this](const QColor& color) { setWatermarkColor(color); },
        [this](const QColor& color) {
            if (m_state.m_watermarkConfig.color == color) {
                return;
            }
            m_state.m_watermarkConfig.color = color;
            m_watermarkColorPreviewPending = true;
            notifyWatermarkPreviewChanged();
        },
        editorServices(), metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    m_watermarkTextEdit = new adqt::widgets::AdLineEdit(controls);
    m_watermarkTextEdit->setFocusPolicy(Qt::ClickFocus);
    m_watermarkTextEdit->setObjectName(QStringLiteral("screenshotWatermarkTextEdit"));
    setScreenshotToolPalettePlaceholderSource(m_watermarkTextEdit, "Watermark text");
    setScreenshotToolPaletteTooltipSource(m_watermarkTextEdit, "Watermark text");
    setScreenshotToolPaletteAccessibleNameSource(m_watermarkTextEdit, "Watermark text");
    const QString watermarkTextLabel =
        QCoreApplication::translate("ScreenshotToolPalette", "Watermark text");
    m_watermarkTextEdit->setPlaceholderText(watermarkTextLabel);
    m_watermarkTextEdit->setToolTip(watermarkTextLabel);
    m_watermarkTextEdit->setAccessibleName(watermarkTextLabel);
    m_watermarkTextEdit->setControlSize(adqt::widgets::AdLineEdit::ControlSize::Small);
    m_watermarkTextEdit->setVariant(adqt::widgets::AdLineEdit::Variant::Borderless);
    m_watermarkTextEdit->setFixedSize(
        qMax(1, qRound(static_cast<qreal>(kWatermarkTextWidth) * metrics.physicalScale)),
        qMax(1, qRound(metrics.buttonSize * metrics.physicalScale)));
    stampScreenshotToolbarReferenceWidth(m_watermarkTextEdit, kWatermarkTextWidth);
    m_watermarkTextEdit->setText(config.text);
    layout->addWidget(m_watermarkTextEdit);
    QObject::connect(m_watermarkTextEdit, &QLineEdit::textChanged, controls,
                     [this](const QString& text) {
                         const QString normalized = text.trimmed();
                         if (m_state.m_watermarkConfig.text == normalized) {
                             return;
                         }
                         m_state.m_watermarkConfig.text = normalized;
                         notifyWatermarkConfigChanged();
                     });
    QObject::connect(m_watermarkTextEdit, &QLineEdit::editingFinished, controls, [this]() {
        const QString normalized = m_watermarkTextEdit->text().trimmed();
        m_state.m_watermarkConfig.text = normalized;
        const QSignalBlocker blocker(m_watermarkTextEdit);
        m_watermarkTextEdit->setText(normalized);
    });

    ScreenshotToolPaletteFontEditorConfig watermarkFontConfig;
    watermarkFontConfig.accessibleName = QStringLiteral("Watermark font family");
    watermarkFontConfig.summaryTooltip = QStringLiteral("Current watermark font size");
    watermarkFontConfig.summaryObjectName =
        QStringLiteral("screenshotWatermarkFontSizeSummaryButton");
    watermarkFontConfig.sizeValues = style_presets::watermarkFontSizes();
    watermarkFontConfig.observePopup = true;
    watermarkFontConfig.presetTooltip = [](int index, double value) {
        return style_presets::sizePresetTooltip("Watermark font size %1 (%2px)", index, value);
    };
    m_watermarkFontEditor = std::make_unique<ScreenshotToolPaletteFontEditor>();
    registerEditor(m_watermarkFontEditor.get());
    m_watermarkFontEditor->build(
        layout, controls, controls, watermarkFontConfig, config.fontSize, config.fontFamily,
        [this]() { cycleWatermarkFontSize(); },
        [this](double value) { setWatermarkFontSize(value); },
        [this](const QString& value) { setWatermarkFontFamily(value); }, editorServices(), metrics);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    m_watermarkAngleEditor = createScreenshotToolPaletteIconNumericValueButton(
        controls, "Watermark angle", custom_outlined_icons::Angle(),
        qBound(-90, qRound(config.angle), 90), QStringLiteral("-90"), metrics);
    m_watermarkAngleEditor->setObjectName(QStringLiteral("screenshotWatermarkAngleEditor"));
    layout->addWidget(m_watermarkAngleEditor);
    QObject::connect(m_watermarkAngleEditor, &adqt::widgets::AdButton::clicked, controls,
                     [this]() { setWatermarkAngle(30.0); });

    m_watermarkGapEditor = createScreenshotToolPaletteIconNumericValueButton(
        controls, "Watermark gap", custom_outlined_icons::WatermarkGap(),
        qBound(10, qRound(config.gap), 200), QStringLiteral("200"), metrics);
    m_watermarkGapEditor->setObjectName(QStringLiteral("screenshotWatermarkGapEditor"));
    layout->addWidget(m_watermarkGapEditor);
    QObject::connect(m_watermarkGapEditor, &adqt::widgets::AdButton::clicked, controls,
                     [this]() { setWatermarkGap(56.0); });

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    ScreenshotToolPaletteSliderEditorConfig opacityConfig;
    opacityConfig.iconObjectName = QStringLiteral("screenshotWatermarkOpacityIcon");
    opacityConfig.sliderObjectName = QStringLiteral("screenshotWatermarkOpacitySlider");
    opacityConfig.accessibleName = QStringLiteral("Opacity");
    opacityConfig.sliderTooltip = QStringLiteral("Adjust opacity");
    opacityConfig.iconRef = custom_outlined_icons::Opacity();
    opacityConfig.initialValue = qRound(std::clamp(config.opacity, 0.0, 1.0) * 100.0);
    opacityConfig.baseIconSize = metrics.iconSize;
    opacityConfig.baseSliderWidth = kOpacitySliderWidth;
    m_watermarkOpacityEditor =
        createScreenshotToolPaletteSliderEditor(layout, controls, opacityConfig, metrics);
    QObject::connect(m_watermarkOpacityEditor.slider, &adqt::widgets::AdSlider::valueChanged,
                     controls, [this](double value) { setWatermarkOpacity(value / 100.0); });
    refreshWatermarkOpacityMetrics(metrics);

    registerWatermarkEntries();
    updateWatermarkControls();
    return controls;
}

ScreenshotToolPaletteFilterFamilyResult ScreenshotToolPaletteStyleControls::buildFilterFamily(
    const ScreenshotToolPaletteFilterFamilyConfig& config,
    const ScreenshotToolPaletteFilterCallbacks& callbacks, QWidget* panel,
    const ScreenshotToolPaletteStyleFamilyHost& host,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    ScreenshotToolPaletteFilterFamilyResult result;
    if (panel == nullptr) {
        return result;
    }
    result.controls = createRowWidget(panel, config.controlsObjectName, host);
    auto* layout = static_cast<QHBoxLayout*>(result.controls->layout());

    const QVector<ScreenshotToolPaletteStyleModeSelectorOption> modes{
        {static_cast<int>(ScreenshotToolPalette::Tool::PenFilter), QStringLiteral("Pen filter"),
         outlined_icons::Highlight()},
        {static_cast<int>(ScreenshotToolPalette::Tool::RectangleFilter),
         QStringLiteral("Rectangle filter"), custom_outlined_icons::ShapeRectangle()},
    };
    if (host.createModeSelector) {
        layout->addWidget(host.createModeSelector(
            result.controls, QStringLiteral("screenshotFilterModeSelector"),
            static_cast<int>(ScreenshotToolPalette::Tool::PenFilter), modes));
    }
    if (host.addGroupSpacing) {
        host.addGroupSpacing(layout);
    }

    ScreenshotToolPaletteSelectEditorConfig typeSelectConfig;
    typeSelectConfig.objectName = config.typeSelectObjectName;
    typeSelectConfig.accessibleName = QStringLiteral("Filter type");
    typeSelectConfig.tooltip = QStringLiteral("Filter type");
    typeSelectConfig.placeholder = QStringLiteral("Filter type");
    const ScreenshotToolPaletteSelectEditor typeSelectEditor =
        createScreenshotToolPaletteSelectEditor(result.controls, typeSelectConfig, metrics);
    result.typeSelect = typeSelectEditor.select;
    result.typeSelect->setSortComparator(
        [](const adqt::widgets::AdSelect::Option& lhs, const adqt::widgets::AdSelect::Option& rhs) {
            return lhs.value.toInt() < rhs.value.toInt();
        });
    auto* typeModel = new QStandardItemModel(result.typeSelect);
    const auto appendFilterType = [typeModel](const char* source, int value) {
        const ScreenshotToolPaletteTranslationText text(source);
        auto* item = new QStandardItem(text.translated());
        setScreenshotToolPaletteItemTranslationSource(item, text);
        item->setData(value, adqt::widgets::AdSelect::DefaultValueRole);
        typeModel->appendRow(item);
    };
    appendFilterType("Mosaic", 0);
    appendFilterType("Gaussian blur", 1);
    appendFilterType("Grayscale", 2);
    appendFilterType("Inversion", 3);
    result.typeSelect->setModel(typeModel);
    layout->addWidget(result.typeSelect);

    if (host.addGroupSeparator) {
        host.addGroupSeparator(layout);
    }

    if (config.includeStrokeWidth) {
        ScreenshotToolPaletteNumericPresetEditorConfig penFilterWidthConfig =
            snow_shot::presentation::screenshotToolPaletteSizePresetEditorConfig(
                QStringLiteral("Current pen filter stroke width"),
                QStringLiteral("screenshotPenFilterStrokeWidthSummary"),
                "Pen filter stroke width %1 (%2px)");
        penFilterWidthConfig.presetObjectName = [](double value) {
            return QStringLiteral("screenshotPenFilterStrokeWidth%1").arg(qRound(value));
        };
        m_penFilterStrokeWidthEditor = std::make_unique<ScreenshotToolPaletteNumericPresetEditor>();
        registerEditor(m_penFilterStrokeWidthEditor.get());
        m_penFilterStrokeWidthEditor->build(layout, result.controls, result.controls,
                                            penFilterWidthConfig, config.initialStrokeWidth,
                                            callbacks.cycleStrokeWidth, callbacks.setStrokeWidth,
                                            metrics);
        if (host.addGroupSeparator) {
            host.addGroupSeparator(layout);
        }
    }

    ScreenshotToolPaletteSliderEditorConfig intensityConfig;
    intensityConfig.iconObjectName = config.intensityIconObjectName;
    intensityConfig.sliderObjectName = config.intensitySliderObjectName;
    intensityConfig.accessibleName = QStringLiteral("Filter intensity");
    intensityConfig.sliderTooltip = QStringLiteral("Adjust filter intensity");
    intensityConfig.iconRef = custom_outlined_icons::Blur();
    intensityConfig.initialValue = 50;
    intensityConfig.baseIconSize = kCompactSliderIconSize;
    intensityConfig.baseSliderWidth = kCompactSliderWidth;
    const ScreenshotToolPaletteSliderEditor intensityEditor =
        createScreenshotToolPaletteSliderEditor(layout, result.controls, intensityConfig, metrics);
    result.intensityIcon = intensityEditor.icon;
    result.intensitySlider = intensityEditor.slider;

    QObject::connect(result.typeSelect, &adqt::widgets::AdSelect::currentValueChanged,
                     result.controls, [callbacks](const QVariant& value) {
                         if (!value.canConvert<int>()) {
                             return;
                         }
                         if (callbacks.setType) {
                             callbacks.setType(value.toInt());
                         }
                     });
    QObject::connect(result.intensitySlider, &adqt::widgets::AdSlider::valueChanged,
                     result.controls, [callbacks](double value) {
                         if (callbacks.setStrength) {
                             callbacks.setStrength(qBound(0.0, value / 100.0, 1.0));
                         }
                     });
    return result;
}

ScreenshotToolPaletteEditorServices ScreenshotToolPaletteStyleControls::editorServices() {
    ScreenshotToolPaletteEditorServices services;
    services.canvasColorSamplingRequested = m_callbacks.canvasColorSamplingRequested;
    services.popupInteractionBegan = [this](QObject* popup) {
        beginTextStylePopupInteraction(popup);
    };
    services.popupInteractionEnded = [this](QObject* popup) {
        endTextStylePopupInteraction(popup);
    };
    return services;
}

void ScreenshotToolPaletteStyleControls::applyEditorEntries(
    const QVector<StyleEditorEntry>& entries, quint32 groups) {
    for (const StyleEditorEntry& entry : entries) {
        if ((groups & entry.groupMask) != 0 && entry.applyState) {
            entry.applyState();
        }
    }
}

void ScreenshotToolPaletteStyleControls::refreshEditorEntries(QVector<StyleEditorEntry>& entries,
                                                              quint32 groups, quint32 allGroups) {
#if defined(SNOW_SHOT_TEST_HOOKS)
    m_propertyGroupRefreshCount += propertyGroupCount(groups, allGroups);
#else
    static_cast<void>(allGroups);
#endif
    applyEditorEntries(entries, groups);
}

void ScreenshotToolPaletteStyleControls::registerShapeEntries() {
    const auto mixed = [this](quint32 property) { return hasMixedProperty(property); };
    m_shapeEntries = {
        {ShapeModeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.mode_refresh");
             if (m_shapeButtonGroup == nullptr) {
                 return;
             }
             const QSignalBlocker blocker(m_shapeButtonGroup);
             const auto shape = activeShapeStyle().shape();
             m_shapeButtonGroup->setCheckedId(shape == SnowCanvasRectangleShape::Ellipse   ? 1
                                              : shape == SnowCanvasRectangleShape::Diamond ? 2
                                                                                           : 0);
         }},
        {ShapeStrokeWidthRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.stroke_width_refresh");
             if (m_shapeStrokeWidthEditor != nullptr) {
                 m_shapeStrokeWidthEditor->update(activeShapeStyle().strokeWidth(),
                                                  mixed(SnowCanvasShapeStylePropertyStrokeWidth));
             }
         }},
        {ShapeStrokeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.stroke_refresh");
             if (m_shapeStrokeEditor != nullptr) {
                 const auto& style = activeShapeStyle();
                 m_shapeStrokeEditor->update(style.strokeColor(), style.strokeStyle(),
                                             mixed(SnowCanvasShapeStylePropertyStrokeColor),
                                             mixed(SnowCanvasShapeStylePropertyStrokeStyle));
             }
         }},
        {ShapeFillRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.fill_refresh");
             if (m_shapeFillEditor != nullptr) {
                 const auto& style = activeShapeStyle();
                 m_shapeFillEditor->update(style.fillColor(), style.fillStyle(),
                                           mixed(SnowCanvasShapeStylePropertyFillColor),
                                           mixed(SnowCanvasShapeStylePropertyFillStyle));
             }
         }},
        {ShapeCornerRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.corner_refresh");
             if (m_cornerRadiusEditor != nullptr) {
                 m_cornerRadiusEditor->setCornerRadius(activeShapeStyle().cornerRadius());
                 m_cornerRadiusEditor->setMixed(mixed(SnowCanvasShapeStylePropertyCornerRadius));
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::registerHighlightEntries() {
    const auto mixed = [this](quint32 property) { return hasMixedProperty(property); };
    m_highlightEntries = {
        {ShapeFillRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.highlight.fill_refresh");
             if (m_highlightColorEditor != nullptr) {
                 m_highlightColorEditor->update(m_state.m_highlightStyle.fillColor(),
                                                mixed(SnowCanvasShapeStylePropertyFillColor));
             }
         }},
        {ShapeStrokeRefresh | ShapeStrokeWidthRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.highlight.stroke_refresh");
             if (m_highlightStrokeEditor != nullptr) {
                 m_highlightStrokeEditor->update(m_state.m_highlightStyle.strokeWidth(),
                                                 m_state.m_highlightStyle.strokeColor(),
                                                 mixed(SnowCanvasShapeStylePropertyStrokeWidth),
                                                 mixed(SnowCanvasShapeStylePropertyStrokeColor));
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::registerPenHighlightEntries() {
    const auto mixed = [this](quint32 property) { return hasMixedProperty(property); };
    m_penHighlightEntries = {
        {ShapeStrokeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.pen_highlight.color_refresh");
             if (m_penHighlightColorEditor != nullptr) {
                 m_penHighlightColorEditor->update(m_state.m_penHighlightStyle.stroke,
                                                   mixed(SnowCanvasShapeStylePropertyStrokeColor));
             }
         }},
        {ShapeStrokeWidthRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.pen_highlight.width_refresh");
             if (m_penHighlightStrokeWidthEditor != nullptr) {
                 m_penHighlightStrokeWidthEditor->update(
                     m_state.m_penHighlightStyle.strokeWidth,
                     mixed(SnowCanvasShapeStylePropertyStrokeWidth));
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::registerArrowEntries() {
    const auto mixed = [this](quint32 property) { return hasMixedProperty(property); };
    m_arrowEntries = {
        {ShapeStrokeWidthRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.stroke_width_refresh");
             if (m_arrowStrokeWidthEditor != nullptr) {
                 m_arrowStrokeWidthEditor->update(m_state.m_arrowStyle.strokeWidth,
                                                  mixed(SnowCanvasShapeStylePropertyStrokeWidth));
             }
         }},
        {ShapeStrokeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.stroke_refresh");
             if (m_arrowStrokeEditor != nullptr) {
                 m_arrowStrokeEditor->update(m_state.m_arrowStyle.stroke,
                                             m_state.m_arrowStyle.strokeStyle,
                                             mixed(SnowCanvasShapeStylePropertyStrokeColor),
                                             mixed(SnowCanvasShapeStylePropertyStrokeStyle));
             }
         }},
        {ShapeArrowTypeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.type_refresh");
             if (m_arrowTypeButtonGroup == nullptr) {
                 return;
             }
             const QSignalBlocker blocker(m_arrowTypeButtonGroup);
             m_arrowTypeButtonGroup->setCheckedId(
                 mixed(SnowCanvasShapeStylePropertyArrowType)                   ? -1
                 : m_state.m_arrowStyle.arrowType == SnowCanvasArrowType::Curve ? 1
                 : m_state.m_arrowStyle.arrowType == SnowCanvasArrowType::Elbow ? 2
                                                                                : 0);
         }},
        {ShapeArrowheadsRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.arrowheads_refresh");
             if (m_startArrowheadEditor != nullptr) {
                 m_startArrowheadEditor->update(
                     static_cast<int>(m_state.m_arrowStyle.startArrowhead),
                     mixed(SnowCanvasShapeStylePropertyStartArrowhead));
             }
             if (m_endArrowheadEditor != nullptr) {
                 m_endArrowheadEditor->update(static_cast<int>(m_state.m_arrowStyle.endArrowhead),
                                              mixed(SnowCanvasShapeStylePropertyEndArrowhead));
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::registerTextEntries() {
    const auto mixed = [this](quint32 property) {
        return m_state.m_showingSelectedTextStyle && (m_state.m_textStyleMixed & property) != 0;
    };
    m_textEntries = {
        {TextColorRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.color_refresh");
             if (m_textColorEditor != nullptr) {
                 m_textColorEditor->update(m_state.m_textStyle.textStyle().color,
                                           mixed(SnowCanvasTextStyleMixedColor));
             }
         }},
        {TextFontSizeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.font_size_refresh");
             if (m_textFontEditor != nullptr) {
                 const SnowCanvasTextStyle& style = m_state.m_textStyle.textStyle();
                 m_textFontEditor->update(
                     style.fontSize, style.fontFamily, mixed(SnowCanvasTextStyleMixedFontSize),
                     mixed(SnowCanvasTextStyleMixedFontFamily), TextFontSizeRefresh,
                     TextFontSizeRefresh, TextFontFamilyRefresh);
             }
         }},
        {TextFontFamilyRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.font_family_refresh");
             if (m_textFontEditor != nullptr) {
                 const SnowCanvasTextStyle& style = m_state.m_textStyle.textStyle();
                 m_textFontEditor->update(
                     style.fontSize, style.fontFamily, mixed(SnowCanvasTextStyleMixedFontSize),
                     mixed(SnowCanvasTextStyleMixedFontFamily), TextFontFamilyRefresh,
                     TextFontSizeRefresh, TextFontFamilyRefresh);
             }
         }},
        {TextStrokeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.stroke_refresh");
             if (m_textStrokeEditor != nullptr) {
                 const SnowCanvasTextStyle& style = m_state.m_textStyle.textStyle();
                 m_textStrokeEditor->update(style.strokeWidth, style.stroke,
                                            mixed(SnowCanvasTextStyleMixedStrokeWidth),
                                            mixed(SnowCanvasTextStyleMixedStroke));
             }
         }},
        {TextFillRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.fill_refresh");
             if (m_textFillEditor != nullptr) {
                 const SnowCanvasTextStyle& style = m_state.m_textStyle.textStyle();
                 m_textFillEditor->update(style.fill, style.fillStyle,
                                          mixed(SnowCanvasTextStyleMixedFill),
                                          mixed(SnowCanvasTextStyleMixedFillStyle));
             }
         }},
        {TextCornerRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.corner_refresh");
             if (m_textCornerRadiusEditor == nullptr) {
                 return;
             }
             const SnowCanvasTextStyle& style = m_state.m_textStyle.textStyle();
             const bool uniform = style.cornerRadii.topLeft == style.cornerRadii.topRight &&
                                  style.cornerRadii.topLeft == style.cornerRadii.bottomRight &&
                                  style.cornerRadii.topLeft == style.cornerRadii.bottomLeft;
             m_textCornerRadiusEditor->setCornerRadius(qRound(style.cornerRadii.topLeft));
             m_textCornerRadiusEditor->setMixed(mixed(SnowCanvasTextStyleMixedCornerRadii) ||
                                                !uniform);
         }},
        {TextAlignmentRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.alignment_refresh");
             if (m_textAlignmentEditor != nullptr) {
                 m_textAlignmentEditor->update(
                     static_cast<int>(m_state.m_textStyle.textStyle().horizontalAlign),
                     mixed(SnowCanvasTextStyleMixedHorizontalAlign));
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::registerSerialNumberEntries() {
    const auto mixed = [this](quint32 property) {
        return (m_state.m_serialNumberStyleMixed & property) != 0;
    };
    m_serialNumberEntries = {
        {SerialNumberColorRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.color_refresh");
             if (m_serialNumberColorEditor != nullptr) {
                 m_serialNumberColorEditor->update(m_state.m_serialNumberStyle.color,
                                                   mixed(SnowCanvasSerialNumberStyleMixedColor));
             }
         }},
        {SerialNumberFillRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.fill_refresh");
             if (m_serialNumberFillEditor != nullptr) {
                 m_serialNumberFillEditor->update(m_state.m_serialNumberStyle.fill,
                                                  m_state.m_serialNumberStyle.fillStyle,
                                                  mixed(SnowCanvasSerialNumberStyleMixedFill),
                                                  mixed(SnowCanvasSerialNumberStyleMixedFillStyle));
             }
         }},
        {SerialNumberValueRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.value_refresh");
             if (m_serialNumberEditor != nullptr) {
                 m_serialNumberEditor->setCornerRadius(static_cast<int>(std::clamp<qint64>(
                     m_state.m_serialNumberStyle.number, 0, std::numeric_limits<int>::max())));
                 m_serialNumberEditor->setMixed(mixed(SnowCanvasSerialNumberStyleMixedNumber));
             }
         }},
        {SerialNumberFontSizeRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.font_size_refresh");
             if (m_serialNumberFontEditor != nullptr) {
                 m_serialNumberFontEditor->update(
                     m_state.m_serialNumberStyle.fontSize, m_state.m_serialNumberStyle.fontFamily,
                     mixed(SnowCanvasSerialNumberStyleMixedFontSize),
                     mixed(SnowCanvasSerialNumberStyleMixedFontFamily), SerialNumberFontSizeRefresh,
                     SerialNumberFontSizeRefresh, SerialNumberFontFamilyRefresh);
             }
         }},
        {SerialNumberFontFamilyRefresh,
         [this, mixed]() {
             SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.font_family_refresh");
             if (m_serialNumberFontEditor != nullptr) {
                 m_serialNumberFontEditor->update(
                     m_state.m_serialNumberStyle.fontSize, m_state.m_serialNumberStyle.fontFamily,
                     mixed(SnowCanvasSerialNumberStyleMixedFontSize),
                     mixed(SnowCanvasSerialNumberStyleMixedFontFamily),
                     SerialNumberFontFamilyRefresh, SerialNumberFontSizeRefresh,
                     SerialNumberFontFamilyRefresh);
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::registerWatermarkEntries() {
    // The watermark family has no differential refresh groups; the single
    // entry mirrors the previous full-refresh behavior.
    m_watermarkEntries = {
        {kAllRefreshGroups,
         [this]() {
             const SnowCanvasWatermarkConfig& config = m_state.m_watermarkConfig;
             if (m_watermarkColorEditor != nullptr) {
                 m_watermarkColorEditor->update(config.color, false);
             }
             if (m_watermarkTextEdit != nullptr) {
                 const QSignalBlocker blocker(m_watermarkTextEdit);
                 m_watermarkTextEdit->setText(config.text);
             }
             if (m_watermarkFontEditor != nullptr) {
                 m_watermarkFontEditor->update(config.fontSize, config.fontFamily.trimmed(), false,
                                               false, 0x3u, 0x1u, 0x2u);
             }
             if (m_watermarkAngleEditor != nullptr) {
                 m_watermarkAngleEditor->setValue(qBound(-90, qRound(config.angle), 90));
             }
             if (m_watermarkGapEditor != nullptr) {
                 m_watermarkGapEditor->setValue(qBound(10, qRound(config.gap), 200));
             }
             if (m_watermarkOpacityEditor.slider != nullptr) {
                 const QSignalBlocker blocker(m_watermarkOpacityEditor.slider);
                 m_watermarkOpacityEditor.slider->setValue(
                     qRound(std::clamp(config.opacity, 0.0, 1.0) * 100.0));
                 m_watermarkOpacityEditor.slider->setAccessibleDescription(
                     QStringLiteral("%1%").arg(qRound(m_watermarkOpacityEditor.slider->value())));
             }
         }},
    };
}

void ScreenshotToolPaletteStyleControls::updateArrowStyleControls(quint32 groups) {
    if (groups == 0xffffffffu) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.property_group_refresh");
    }
    refreshEditorEntries(m_arrowEntries, groups, AllShapeRefreshes);
}

void ScreenshotToolPaletteStyleControls::updateRectangleStyleControls(quint32 groups) {
    if (groups == 0xffffffffu) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.property_group_refresh");
    }
    refreshEditorEntries(m_shapeEntries, groups, AllShapeRefreshes);
    // The rectangle-highlight row shares the shape refresh groups but is not
    // itself counted: masked refreshes only reach it while it is active.
    if (groups == 0xffffffffu || m_state.m_highlightControlsActive) {
        updateHighlightStyleControls(groups);
    }
}

void ScreenshotToolPaletteStyleControls::updateHighlightStyleControls(quint32 groups) {
    applyEditorEntries(m_highlightEntries, groups);
}

void ScreenshotToolPaletteStyleControls::updatePenHighlightStyleControls(quint32 groups) {
    if (groups == 0xffffffffu) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.pen_highlight.property_group_refresh");
    }
    refreshEditorEntries(m_penHighlightEntries, groups, AllShapeRefreshes);
}

void ScreenshotToolPaletteStyleControls::updateTextStyleControls(quint32 groups) {
    if (groups == 0xffffffffu) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.property_group_refresh");
    }
    refreshEditorEntries(m_textEntries, groups, AllTextRefreshes);
}

void ScreenshotToolPaletteStyleControls::updateSerialNumberStyleControls(quint32 groups) {
    if (groups == 0xffffffffu) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.property_group_refresh");
    }
    refreshEditorEntries(m_serialNumberEntries, groups, AllSerialNumberRefreshes);
}

void ScreenshotToolPaletteStyleControls::updateWatermarkControls() {
    applyEditorEntries(m_watermarkEntries, kAllRefreshGroups);
}

void ScreenshotToolPaletteStyleControls::updateRectangleOnlyControlsVisibility() {
    const bool visible = !m_state.m_lineControlsActive && !m_state.m_freeDrawControlsActive;
    if (m_shapeControlsContainer != nullptr) {
        m_shapeControlsContainer->setVisible(visible);
    }
    if (m_cornerRadiusEditor != nullptr) {
        m_cornerRadiusEditor->setVisible(visible);
    }
}

void ScreenshotToolPaletteStyleControls::reset() {
    clearTextStylePopupInteractions();
    m_state.reset(m_defaults);
    updateRectangleStyleControls();
    updateArrowStyleControls();
    updateTextStyleControls();
    updateWatermarkControls();
    updateSerialNumberStyleControls();
}

void ScreenshotToolPaletteStyleControls::releaseControlBindings() {
    clearTextStylePopupInteractions();

    m_state.m_arrowControlsActive = false;
    m_state.m_lineControlsActive = false;
    m_state.m_freeDrawControlsActive = false;
    m_state.m_highlightControlsActive = false;
    m_state.m_penHighlightControlsActive = false;
    m_state.m_textControlsActive = false;

    m_shapeStrokeWidthEditor.reset();
    m_shapeStrokeEditor.reset();
    m_shapeFillEditor.reset();
    m_cornerRadiusEditor = nullptr;
    m_shapeControlsContainer = nullptr;
    m_highlightColorEditor.reset();
    m_spotlightColorEditor.reset();
    m_shapeButtonGroup = nullptr;
    m_highlightStrokeEditor.reset();
    m_penHighlightColorEditor.reset();
    m_penHighlightStrokeWidthEditor.reset();
    m_penFilterStrokeWidthEditor.reset();
    m_arrowStrokeWidthEditor.reset();
    m_arrowStrokeEditor.reset();
    m_arrowTypeButtonGroup = nullptr;
    m_startArrowheadEditor.reset();
    m_endArrowheadEditor.reset();
    m_textColorEditor.reset();
    m_textFontEditor.reset();
    m_textStrokeEditor.reset();
    m_textFillEditor.reset();
    m_textCornerRadiusEditor = nullptr;
    m_textAlignmentEditor.reset();
    m_serialNumberColorEditor.reset();
    m_serialNumberFillEditor.reset();
    m_serialNumberEditor = nullptr;
    m_serialNumberFontEditor.reset();
    m_watermarkColorPreviewPending = false;
    m_watermarkColorEditor.reset();
    m_watermarkTextEdit = nullptr;
    m_watermarkFontEditor.reset();
    m_watermarkAngleEditor = nullptr;
    m_watermarkGapEditor = nullptr;
    m_watermarkOpacityEditor = {};
    m_spotlightOpacityEditor = {};

    m_registeredComponents.clear();
    m_shapeEntries.clear();
    m_highlightEntries.clear();
    m_penHighlightEntries.clear();
    m_arrowEntries.clear();
    m_textEntries.clear();
    m_serialNumberEntries.clear();
    m_watermarkEntries.clear();
    m_toolbarSpacingItems.clear();
}

int ScreenshotToolPaletteStyleControls::spacerReferenceWidth(const QSpacerItem* spacer) const {
    if (spacer == nullptr) {
        return 0;
    }
    for (const ToolbarSpacingItem& item : m_toolbarSpacingItems) {
        if (item.item == spacer) {
            return item.baseSpacing;
        }
    }
    return 0;
}

void ScreenshotToolPaletteStyleControls::setCreationStyleDefaults(
    const SnowCanvasStyleDefaults& defaults) {
    const SnowCanvasWatermarkConfig watermark = m_state.m_watermarkConfig;
    const SnowCanvasSpotlightConfig spotlight = m_state.spotlightConfig;
    m_state.reset(defaults);
    m_state.m_watermarkConfig = watermark;
    m_state.spotlightConfig = spotlight;
    updateRectangleStyleControls();
    updateArrowStyleControls();
    updateHighlightStyleControls();
    updatePenHighlightStyleControls();
    updateTextStyleControls();
    updateSerialNumberStyleControls();
}

void ScreenshotToolPaletteStyleControls::setArrowControlsActive(bool active) {
    if (m_state.m_arrowControlsActive == active) {
        return;
    }
    m_state.m_arrowControlsActive = active;
}

void ScreenshotToolPaletteStyleControls::setLineControlsActive(bool active) {
    if (m_state.m_lineControlsActive == active) {
        return;
    }
    m_state.m_lineControlsActive = active;
    updateRectangleOnlyControlsVisibility();
    if (active) {
        updateRectangleStyleControls();
    }
}

void ScreenshotToolPaletteStyleControls::setFreeDrawControlsActive(bool active) {
    if (m_state.m_freeDrawControlsActive == active) {
        return;
    }
    m_state.m_freeDrawControlsActive = active;
    updateRectangleOnlyControlsVisibility();
    if (active) {
        updateRectangleStyleControls();
    }
}

void ScreenshotToolPaletteStyleControls::setHighlightControlsActive(bool active) {
    if (m_state.m_highlightControlsActive == active) {
        return;
    }
    m_state.m_highlightControlsActive = active;
    updateHighlightStyleControls();
}

void ScreenshotToolPaletteStyleControls::setPenHighlightControlsActive(bool active) {
    if (m_state.m_penHighlightControlsActive == active) {
        return;
    }
    m_state.m_penHighlightControlsActive = active;
    updatePenHighlightStyleControls();
}

void ScreenshotToolPaletteStyleControls::setTextControlsActive(bool active) {
    if (m_state.m_textControlsActive == active) {
        return;
    }
    if (!active) {
        clearTextStylePopupInteractions();
    }
    m_state.m_textControlsActive = active;
}

void ScreenshotToolPaletteStyleControls::beginTextStylePopupInteraction(QObject* popup) {
    if (popup == nullptr || m_openTextStylePopups.contains(popup)) {
        return;
    }
    const bool wasEmpty = m_openTextStylePopups.isEmpty();
    m_openTextStylePopups.insert(popup);
    if (wasEmpty && m_callbacks.textStylePopupInteractionBegan) {
        m_callbacks.textStylePopupInteractionBegan();
    }
}

void ScreenshotToolPaletteStyleControls::endTextStylePopupInteraction(QObject* popup) {
    if (popup == nullptr || !m_openTextStylePopups.remove(popup)) {
        return;
    }
    if (m_openTextStylePopups.isEmpty() && m_callbacks.textStylePopupInteractionEnded) {
        m_callbacks.textStylePopupInteractionEnded();
    }
}

void ScreenshotToolPaletteStyleControls::clearTextStylePopupInteractions() {
    if (m_openTextStylePopups.isEmpty()) {
        return;
    }
    m_openTextStylePopups.clear();
    if (m_callbacks.textStylePopupInteractionEnded) {
        m_callbacks.textStylePopupInteractionEnded();
    }
}

bool ScreenshotToolPaletteStyleControls::stepStrokeWidth(int direction) {
    if (m_state.m_penHighlightControlsActive) {
        if (direction == 0) {
            return false;
        }
        setPenHighlightStrokeWidth(m_state.m_penHighlightStyle.strokeWidth +
                                   (direction > 0 ? 1.0 : -1.0));
        return true;
    }

    if (m_state.m_arrowControlsActive) {
        if (direction == 0) {
            return false;
        }

        const double strokeWidth =
            std::clamp(m_state.m_arrowStyle.strokeWidth + (direction > 0 ? 1.0 : -1.0), 1.0, 72.0);
        if (qFuzzyCompare(strokeWidth + 1.0, m_state.m_arrowStyle.strokeWidth + 1.0) &&
            !hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth)) {
            return true;
        }

        setArrowStrokeWidth(strokeWidth);
        return true;
    }

    const bool strokeWidthMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    const double previousStrokeWidth = style.strokeWidth();
    if (!style.stepStrokeWidth(direction)) {
        return false;
    }

    if (strokeWidthMixed || !qFuzzyCompare(previousStrokeWidth + 1.0, style.strokeWidth() + 1.0)) {
        static_cast<void>(creationStyle.setStrokeWidth(style.strokeWidth()));
        clearMixedProperties(SnowCanvasShapeStylePropertyStrokeWidth);
        updateRectangleStyleControls();
        notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyStrokeWidth,
                                activeShapeKind());
    }
    return true;
}

bool ScreenshotToolPaletteStyleControls::stepTextFontSize(int direction) {
    const bool wasMixed = m_state.m_showingSelectedTextStyle &&
                          (m_state.m_textStyleMixed & SnowCanvasTextStyleMixedFontSize) != 0;
    if (!m_state.m_textStyle.stepFontSize(direction) && !wasMixed) {
        return false;
    }
    static_cast<void>(
        m_state.m_creationTextStyle.setFontSize(m_state.m_textStyle.textStyle().fontSize));
    m_state.m_textStyleMixed &= ~SnowCanvasTextStyleMixedFontSize;
    updateTextStyleControls();
    notifyTextStyleChanged();
    return true;
}

bool ScreenshotToolPaletteStyleControls::handleCornerRadiusWheel(const QPoint& globalPosition,
                                                                 int direction) {
    if (direction == 0 || m_cornerRadiusEditor == nullptr || !m_cornerRadiusEditor->isVisible() ||
        !m_cornerRadiusEditor->rect().contains(
            m_cornerRadiusEditor->mapFromGlobal(globalPosition))) {
        return false;
    }

    setCornerRadius(m_state.m_rectangleStyle.cornerRadius() + (direction > 0 ? 1 : -1));
    return true;
}

bool ScreenshotToolPaletteStyleControls::handleTextStrokeWidthWheel(const QPoint& globalPosition,
                                                                    int direction) {
    const bool hasPicker = m_textStrokeEditor != nullptr &&
                           m_textStrokeEditor->picker() != nullptr &&
                           m_textStrokeEditor->picker()->isVisible() &&
                           m_textStrokeEditor->picker()->rect().contains(
                               m_textStrokeEditor->picker()->mapFromGlobal(globalPosition));
    if (direction == 0 || !hasPicker) {
        return false;
    }
    const bool wasMixed = m_state.m_showingSelectedTextStyle &&
                          (m_state.m_textStyleMixed & SnowCanvasTextStyleMixedStrokeWidth) != 0;
    if (m_state.m_textStyle.stepStrokeWidth(direction) || wasMixed) {
        static_cast<void>(m_state.m_creationTextStyle.setStrokeWidth(
            m_state.m_textStyle.textStyle().strokeWidth));
        m_state.m_textStyleMixed &= ~SnowCanvasTextStyleMixedStrokeWidth;
        updateTextStyleControls();
        notifyTextStyleChanged();
    }
    return true;
}

bool ScreenshotToolPaletteStyleControls::handleTextCornerRadiusWheel(const QPoint& globalPosition,
                                                                     int direction) {
    if (direction == 0 || m_textCornerRadiusEditor == nullptr ||
        !m_textCornerRadiusEditor->isVisible() ||
        !m_textCornerRadiusEditor->rect().contains(
            m_textCornerRadiusEditor->mapFromGlobal(globalPosition))) {
        return false;
    }

    setTextCornerRadius(qRound(m_state.m_textStyle.textStyle().cornerRadii.topLeft) +
                        (direction > 0 ? 1 : -1));
    return true;
}

bool ScreenshotToolPaletteStyleControls::handleSerialNumberWheel(const QPoint& globalPosition,
                                                                 int direction) {
    if (direction == 0) {
        return false;
    }
    if (m_serialNumberEditor != nullptr &&
        m_serialNumberEditor->rect().contains(
            m_serialNumberEditor->mapFromGlobal(globalPosition))) {
        const qint64 nextNumber =
            direction > 0 ? (m_state.m_serialNumberStyle.number < std::numeric_limits<qint64>::max()
                                 ? m_state.m_serialNumberStyle.number + 1
                                 : m_state.m_serialNumberStyle.number)
                          : std::max<qint64>(0, m_state.m_serialNumberStyle.number - 1);
        setSerialNumber(nextNumber);
        return true;
    }
    if (m_serialNumberFontEditor != nullptr && m_serialNumberFontEditor->sizeSummary() != nullptr &&
        m_serialNumberFontEditor->sizeSummary()->rect().contains(
            m_serialNumberFontEditor->sizeSummary()->mapFromGlobal(globalPosition))) {
        setSerialNumberFontSize(m_state.m_serialNumberStyle.fontSize +
                                (direction > 0 ? 1.0 : -1.0));
        return true;
    }
    return false;
}

bool ScreenshotToolPaletteStyleControls::handleWatermarkWheel(const QPoint& globalPosition,
                                                              int direction) {
    if (direction == 0) {
        return false;
    }
    if (m_watermarkAngleEditor != nullptr && m_watermarkAngleEditor->isVisible() &&
        m_watermarkAngleEditor->rect().contains(
            m_watermarkAngleEditor->mapFromGlobal(globalPosition))) {
        setWatermarkAngle(qBound(-90.0,
                                 static_cast<double>(qRound(m_state.m_watermarkConfig.angle)) +
                                     (direction > 0 ? 1.0 : -1.0),
                                 90.0));
        return true;
    }
    if (m_watermarkGapEditor != nullptr && m_watermarkGapEditor->isVisible() &&
        m_watermarkGapEditor->rect().contains(
            m_watermarkGapEditor->mapFromGlobal(globalPosition))) {
        setWatermarkGap(qBound(10.0,
                               static_cast<double>(qRound(m_state.m_watermarkConfig.gap)) +
                                   (direction > 0 ? 1.0 : -1.0),
                               200.0));
        return true;
    }
    const bool overOpacityIcon = m_watermarkOpacityEditor.icon != nullptr &&
                                 m_watermarkOpacityEditor.icon->isVisible() &&
                                 m_watermarkOpacityEditor.icon->rect().contains(
                                     m_watermarkOpacityEditor.icon->mapFromGlobal(globalPosition));
    const bool overOpacitySlider =
        m_watermarkOpacityEditor.slider != nullptr &&
        m_watermarkOpacityEditor.slider->isVisible() &&
        m_watermarkOpacityEditor.slider->rect().contains(
            m_watermarkOpacityEditor.slider->mapFromGlobal(globalPosition));
    if (m_watermarkOpacityEditor.slider != nullptr && (overOpacityIcon || overOpacitySlider)) {
        const int current = qRound(m_watermarkOpacityEditor.slider->value());
        m_watermarkOpacityEditor.slider->setValue(
            std::clamp(current + (direction > 0 ? 5 : -5), 0, 100));
        return true;
    }
    return stepWatermarkFontSize(direction);
}

bool ScreenshotToolPaletteStyleControls::stepWatermarkFontSize(int direction) {
    if (direction == 0 || !std::isfinite(m_state.m_watermarkConfig.fontSize)) {
        return false;
    }

    setWatermarkFontSize(
        std::clamp(m_state.m_watermarkConfig.fontSize + (direction > 0 ? 1.0 : -1.0),
                   kMinWatermarkFontSize, kMaxWatermarkFontSize));
    return true;
}

SnowCanvasShapeStyle ScreenshotToolPaletteStyleControls::rectangleStyle() const {
    return m_state.m_rectangleStyle.rectangleStyle();
}

ScreenshotToolPaletteRectangleStyleModel& ScreenshotToolPaletteStyleControls::activeShapeStyle() {
    return m_state.m_highlightControlsActive  ? m_state.m_highlightStyle
           : m_state.m_freeDrawControlsActive ? m_state.m_freeDrawStyle
           : m_state.m_lineControlsActive     ? m_state.m_lineStyle
                                              : m_state.m_rectangleStyle;
}

const ScreenshotToolPaletteRectangleStyleModel&
ScreenshotToolPaletteStyleControls::activeShapeStyle() const {
    return m_state.m_highlightControlsActive  ? m_state.m_highlightStyle
           : m_state.m_freeDrawControlsActive ? m_state.m_freeDrawStyle
           : m_state.m_lineControlsActive     ? m_state.m_lineStyle
                                              : m_state.m_rectangleStyle;
}

ScreenshotToolPaletteRectangleStyleModel&
ScreenshotToolPaletteStyleControls::activeCreationShapeStyle() {
    return m_state.m_highlightControlsActive  ? m_state.m_creationHighlightStyle
           : m_state.m_freeDrawControlsActive ? m_state.m_creationFreeDrawStyle
           : m_state.m_lineControlsActive     ? m_state.m_creationLineStyle
                                              : m_state.m_creationRectangleStyle;
}

SnowCanvasShapeKind ScreenshotToolPaletteStyleControls::activeShapeKind() const {
    return m_state.m_highlightControlsActive  ? SnowCanvasShapeKind::RectangleHighlight
           : m_state.m_freeDrawControlsActive ? SnowCanvasShapeKind::FreeDraw
           : m_state.m_lineControlsActive     ? SnowCanvasShapeKind::Line
                                              : SnowCanvasShapeKind::Rectangle;
}

SnowCanvasStyleDefaults ScreenshotToolPaletteStyleControls::creationStyleDefaults() const {
    SnowCanvasStyleDefaults defaults = m_defaults;
    defaults.rectangle = m_state.m_creationRectangleStyle.rectangleStyle();
    defaults.line = m_state.m_creationLineStyle.rectangleStyle();
    defaults.freeDraw = m_state.m_creationFreeDrawStyle.rectangleStyle();
    defaults.rectangleHighlight = m_state.m_creationHighlightStyle.rectangleStyle();
    defaults.penHighlight = m_state.m_creationPenHighlightStyle;
    defaults.arrow.stroke = m_state.m_creationArrowStyle.stroke;
    defaults.arrow.strokeWidth = m_state.m_creationArrowStyle.strokeWidth;
    defaults.arrow.startArrowhead = m_state.m_creationArrowStyle.startArrowhead;
    defaults.arrow.endArrowhead = m_state.m_creationArrowStyle.endArrowhead;
    defaults.arrow.strokeStyle = m_state.m_creationArrowStyle.strokeStyle;
    defaults.arrow.arrowType = m_state.m_creationArrowStyle.arrowType;
    defaults.text = m_state.m_creationTextStyle.textStyle();
    defaults.serialNumber = m_state.m_creationSerialNumberStyle;
    defaults.rectangleFilter = m_state.creationRectangleFilterStyle;
    defaults.penFilter = m_state.creationPenFilterStyle;
    defaults.watermark = m_state.m_watermarkConfig;
    defaults.spotlight = m_state.spotlightConfig;
    return defaults;
}

void ScreenshotToolPaletteStyleControls::setRectangleStyle(const SnowCanvasShapeStyle& style) {
    m_state.m_creationRectangleStyle.setRectangleStyle(style);
    m_state.m_rectangleStyle = m_state.m_creationRectangleStyle;
    m_state.m_showingSelectedStyle = false;
    m_state.m_selectedStyleMixed = 0;
    updateRectangleStyleControls();
}

void ScreenshotToolPaletteStyleControls::setWatermarkConfig(
    const SnowCanvasWatermarkConfig& config) {
    m_watermarkColorPreviewPending = false;
    if (m_state.m_watermarkConfig == config) {
        return;
    }
    m_state.m_watermarkConfig = config;
    updateWatermarkControls();
}

void ScreenshotToolPaletteStyleControls::setSpotlightConfig(
    const SnowCanvasSpotlightConfig& config) {
    m_state.spotlightConfig = config;
    updateSpotlightColorControls(config.color);
    if (m_spotlightOpacityEditor.slider != nullptr) {
        const QSignalBlocker blocker(m_spotlightOpacityEditor.slider);
        m_spotlightOpacityEditor.slider->setValue(
            qRound(std::clamp(config.opacity, 0.0, 1.0) * 100.0));
        m_spotlightOpacityEditor.slider->setAccessibleDescription(
            QStringLiteral("%1%").arg(qRound(m_spotlightOpacityEditor.slider->value())));
    }
}

void ScreenshotToolPaletteStyleControls::updateSpotlightColorControls(const QColor& color) {
    if (m_spotlightColorEditor != nullptr) {
        m_spotlightColorEditor->update(color, false);
    }
}

adqt::widgets::AdSlider* ScreenshotToolPaletteStyleControls::spotlightOpacitySlider() const {
    return m_spotlightOpacityEditor.slider;
}

QLabel* ScreenshotToolPaletteStyleControls::spotlightOpacityIcon() const {
    return m_spotlightOpacityEditor.icon;
}

void ScreenshotToolPaletteStyleControls::updatePenFilterStrokeWidthControls(double width,
                                                                            bool mixed) {
    if (m_penFilterStrokeWidthEditor != nullptr) {
        m_penFilterStrokeWidthEditor->update(width, mixed);
    }
}

void ScreenshotToolPaletteStyleControls::refreshToolbarMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    const auto applies = [&metrics](const QWidget* widget) {
        return screenshotToolPaletteMetricsApplyTo(metrics, widget);
    };
    for (const ToolbarSpacingItem& item : m_toolbarSpacingItems) {
        if (item.item == nullptr || !screenshotToolPaletteMetricsApplyTo(metrics, item.owner)) {
            continue;
        }
        item.item->changeSize(qMax(1, qRound(item.baseSpacing * metrics.physicalScale)), 0,
                              QSizePolicy::Fixed, QSizePolicy::Minimum);
    }

    configureScreenshotToolPaletteStyleRadioButtonGroup(m_shapeButtonGroup, metrics);
    configureScreenshotToolPaletteStyleRadioButtonGroup(m_arrowTypeButtonGroup, metrics);

    for (ScreenshotToolPaletteStyleEditorComponent* component : m_registeredComponents) {
        component->refreshMetrics(metrics);
    }

    if (applies(m_watermarkTextEdit)) {
        m_watermarkTextEdit->setFixedSize(
            qMax(1, qRound(static_cast<qreal>(kWatermarkTextWidth) * metrics.physicalScale)),
            qMax(1, qRound(metrics.buttonSize * metrics.physicalScale)));
        stampScreenshotToolbarReferenceWidth(m_watermarkTextEdit, kWatermarkTextWidth);
    }
    configureScreenshotToolPaletteCornerRadiusEditor(m_cornerRadiusEditor, metrics);
    configureScreenshotToolPaletteCornerRadiusEditor(m_textCornerRadiusEditor, metrics);
    configureScreenshotToolPaletteCornerRadiusEditor(m_serialNumberEditor, metrics);
    configureScreenshotToolPaletteIconNumericValueButton(m_watermarkAngleEditor, metrics);
    configureScreenshotToolPaletteIconNumericValueButton(m_watermarkGapEditor, metrics);
    refreshWatermarkOpacityMetrics(metrics);
    refreshSpotlightOpacityMetrics(metrics);

    for (ScreenshotToolPaletteStyleEditorComponent* component : m_registeredComponents) {
        component->resetPopupMetrics(metrics);
    }
}

void ScreenshotToolPaletteStyleControls::refreshThemeIcons(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshWatermarkOpacityMetrics(metrics);
    refreshSpotlightOpacityMetrics(metrics);
}

void ScreenshotToolPaletteStyleControls::refreshWatermarkOpacityMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureScreenshotToolPaletteSliderEditor(m_watermarkOpacityEditor, metrics);
}

void ScreenshotToolPaletteStyleControls::refreshSpotlightOpacityMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureScreenshotToolPaletteSliderEditor(m_spotlightOpacityEditor, metrics);
}

void ScreenshotToolPaletteStyleControls::notifyWatermarkConfigChanged() const {
    if (m_callbacks.watermarkConfigChanged) {
        m_callbacks.watermarkConfigChanged(m_state.m_watermarkConfig);
    }
}

void ScreenshotToolPaletteStyleControls::notifyWatermarkPreviewChanged() const {
    if (m_callbacks.watermarkPreviewChanged) {
        m_callbacks.watermarkPreviewChanged(m_state.m_watermarkConfig);
    }
}

void ScreenshotToolPaletteStyleControls::notifyTextStyleChanged() const {
    if (m_callbacks.textStyleChanged) {
        m_callbacks.textStyleChanged(m_state.m_textStyle.textStyle());
    }
}

void ScreenshotToolPaletteStyleControls::notifySerialNumberStyleChanged() const {
    if (m_callbacks.serialNumberStyleChanged) {
        m_callbacks.serialNumberStyleChanged(m_state.m_serialNumberStyle);
    }
}

bool ScreenshotToolPaletteStyleControls::hasMixedProperty(quint32 property) const {
    return m_state.m_showingSelectedStyle && (m_state.m_selectedStyleMixed & property) != 0;
}

void ScreenshotToolPaletteStyleControls::clearMixedProperties(quint32 properties) {
    if (m_state.m_showingSelectedStyle) {
        m_state.m_selectedStyleMixed &= ~properties;
    }
}

void ScreenshotToolPaletteStyleControls::notifyShapeStyleChanged(const SnowCanvasShapeStyle& style,
                                                                 quint32 properties,
                                                                 SnowCanvasShapeKind kind) const {
    if (m_callbacks.shapeStyleChanged) {
        m_callbacks.shapeStyleChanged(style, properties, kind);
    }
}

#if defined(SNOW_SHOT_TEST_HOOKS)
quint64 ScreenshotToolPaletteStyleControls::styleStateNoopCount() const {
    return m_styleStateNoopCount;
}

quint64 ScreenshotToolPaletteStyleControls::propertyGroupRefreshCount() const {
    return m_propertyGroupRefreshCount;
}
#endif

template <typename Apply, typename Mirror>
void ScreenshotToolPaletteStyleControls::commitShapeProperty(quint32 property, Apply apply,
                                                             Mirror mirror) {
    const bool wasMixed = hasMixedProperty(property);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!apply(style) && !wasMixed) {
        return;
    }

    mirror(style, creationStyle);
    clearMixedProperties(property);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), property, activeShapeKind());
}

template <typename Apply>
void ScreenshotToolPaletteStyleControls::commitArrowProperty(quint32 property, Apply apply) {
    const bool wasMixed = hasMixedProperty(property);
    if (!apply(m_state.m_arrowStyle) && !wasMixed) {
        return;
    }
    apply(m_state.m_creationArrowStyle);
    clearMixedProperties(property);
    updateArrowStyleControls();
    notifyShapeStyleChanged(shapeStyleFromArrowStyle(m_state.m_arrowStyle), property,
                            SnowCanvasShapeKind::Arrow);
}

template <typename Apply, typename Mirror>
void ScreenshotToolPaletteStyleControls::commitPenHighlightProperty(quint32 property, Apply apply,
                                                                    Mirror mirror) {
    const bool wasMixed = hasMixedProperty(property);
    if (!apply(m_state.m_penHighlightStyle) && !wasMixed) {
        return;
    }
    mirror(m_state.m_penHighlightStyle, m_state.m_creationPenHighlightStyle);
    clearMixedProperties(property);
    updatePenHighlightStyleControls();
    notifyShapeStyleChanged(m_state.m_penHighlightStyle, property,
                            SnowCanvasShapeKind::PenHighlight);
}

template <typename Apply, typename Mirror>
void ScreenshotToolPaletteStyleControls::commitTextProperty(quint32 mixedFlag, Apply apply,
                                                            Mirror mirror) {
    const bool wasMixed =
        m_state.m_showingSelectedTextStyle && (m_state.m_textStyleMixed & mixedFlag) != 0;
    if (!apply(m_state.m_textStyle) && !wasMixed) {
        return;
    }
    mirror(m_state.m_textStyle, m_state.m_creationTextStyle);
    m_state.m_textStyleMixed &= ~mixedFlag;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

template <typename Apply>
void ScreenshotToolPaletteStyleControls::commitSerialNumberProperty(quint32 mixedFlag,
                                                                    Apply apply) {
    const bool wasMixed = (m_state.m_serialNumberStyleMixed & mixedFlag) != 0;
    if (!apply(m_state.m_serialNumberStyle, m_state.m_creationSerialNumberStyle) && !wasMixed) {
        return;
    }
    m_state.m_serialNumberStyleMixed &= ~mixedFlag;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

template <typename Apply>
void ScreenshotToolPaletteStyleControls::commitWatermarkField(Apply apply) {
    if (!apply(m_state.m_watermarkConfig)) {
        return;
    }
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::setStrokeWidth(double strokeWidth) {
    commitShapeProperty(
        SnowCanvasShapeStylePropertyStrokeWidth,
        [strokeWidth](ScreenshotToolPaletteRectangleStyleModel& style) {
            return style.setStrokeWidth(strokeWidth);
        },
        [](const ScreenshotToolPaletteRectangleStyleModel& style,
           ScreenshotToolPaletteRectangleStyleModel& creation) {
            static_cast<void>(creation.setStrokeWidth(style.strokeWidth()));
        });
}

void ScreenshotToolPaletteStyleControls::cycleStrokeWidth() {
    commitShapeProperty(
        SnowCanvasShapeStylePropertyStrokeWidth,
        [](ScreenshotToolPaletteRectangleStyleModel& style) { return style.cycleStrokeWidth(); },
        [](const ScreenshotToolPaletteRectangleStyleModel& style,
           ScreenshotToolPaletteRectangleStyleModel& creation) {
            static_cast<void>(creation.setStrokeWidth(style.strokeWidth()));
        });
}

void ScreenshotToolPaletteStyleControls::setStrokeColor(const QColor& color) {
    commitShapeProperty(
        SnowCanvasShapeStylePropertyStrokeColor,
        [color](ScreenshotToolPaletteRectangleStyleModel& style) {
            return style.setStrokeColor(color);
        },
        [](const ScreenshotToolPaletteRectangleStyleModel& style,
           ScreenshotToolPaletteRectangleStyleModel& creation) {
            static_cast<void>(creation.setStrokeColor(style.strokeColor()));
        });
}

void ScreenshotToolPaletteStyleControls::setStrokeStyle(SnowCanvasStrokeStyle strokeStyle) {
    commitShapeProperty(
        SnowCanvasShapeStylePropertyStrokeStyle,
        [strokeStyle](ScreenshotToolPaletteRectangleStyleModel& style) {
            return style.setStrokeStyle(strokeStyle);
        },
        [](const ScreenshotToolPaletteRectangleStyleModel& style,
           ScreenshotToolPaletteRectangleStyleModel& creation) {
            static_cast<void>(creation.setStrokeStyle(style.strokeStyle()));
        });
}

void ScreenshotToolPaletteStyleControls::setFillColor(const QColor& color) {
    commitShapeProperty(
        SnowCanvasShapeStylePropertyFillColor,
        [color](ScreenshotToolPaletteRectangleStyleModel& style) {
            return style.setFillColor(color);
        },
        [](const ScreenshotToolPaletteRectangleStyleModel& style,
           ScreenshotToolPaletteRectangleStyleModel& creation) {
            static_cast<void>(creation.setFillColor(style.fillColor()));
        });
}

void ScreenshotToolPaletteStyleControls::setFillStyle(SnowCanvasFillStyle fillStyle) {
    commitShapeProperty(
        SnowCanvasShapeStylePropertyFillStyle,
        [fillStyle](ScreenshotToolPaletteRectangleStyleModel& style) {
            return style.setFillStyle(fillStyle);
        },
        [](const ScreenshotToolPaletteRectangleStyleModel& style,
           ScreenshotToolPaletteRectangleStyleModel& creation) {
            static_cast<void>(creation.setFillStyle(style.fillStyle()));
        });
}

void ScreenshotToolPaletteStyleControls::setCornerRadius(int cornerRadius) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyCornerRadius);
    if (!m_state.m_rectangleStyle.setCornerRadius(cornerRadius) && !wasMixed) {
        return;
    }

    static_cast<void>(
        m_state.m_creationRectangleStyle.setCornerRadius(m_state.m_rectangleStyle.cornerRadius()));
    clearMixedProperties(SnowCanvasShapeStylePropertyCornerRadius);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(m_state.m_rectangleStyle.rectangleStyle(),
                            SnowCanvasShapeStylePropertyCornerRadius,
                            SnowCanvasShapeKind::Rectangle);
}

void ScreenshotToolPaletteStyleControls::setShape(SnowCanvasRectangleShape shape) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyShape);
    if (!m_state.m_rectangleStyle.setShape(shape) && !wasMixed) {
        return;
    }
    static_cast<void>(m_state.m_creationRectangleStyle.setShape(shape));
    clearMixedProperties(SnowCanvasShapeStylePropertyShape);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(m_state.m_rectangleStyle.rectangleStyle(),
                            SnowCanvasShapeStylePropertyShape, SnowCanvasShapeKind::Rectangle);
}

void ScreenshotToolPaletteStyleControls::setPenHighlightColor(const QColor& color) {
    commitPenHighlightProperty(
        SnowCanvasShapeStylePropertyStrokeColor,
        [color](SnowCanvasShapeStyle& style) {
            if (!color.isValid() || style.stroke == color) {
                return false;
            }
            style.stroke = color;
            style.stroke.setAlpha(255);
            return true;
        },
        [](const SnowCanvasShapeStyle& style, SnowCanvasShapeStyle& creation) {
            creation.stroke = style.stroke;
        });
}

void ScreenshotToolPaletteStyleControls::setPenHighlightStrokeWidth(double strokeWidth) {
    const double clampedStrokeWidth = std::clamp(strokeWidth, 1.0, 72.0);
    commitPenHighlightProperty(
        SnowCanvasShapeStylePropertyStrokeWidth,
        [clampedStrokeWidth](SnowCanvasShapeStyle& style) {
            if (qFuzzyCompare(style.strokeWidth + 1.0, clampedStrokeWidth + 1.0)) {
                return false;
            }
            style.strokeWidth = clampedStrokeWidth;
            return true;
        },
        [](const SnowCanvasShapeStyle& style, SnowCanvasShapeStyle& creation) {
            creation.strokeWidth = style.strokeWidth;
        });
}

void ScreenshotToolPaletteStyleControls::setArrowStrokeWidth(double strokeWidth) {
    const double clampedStrokeWidth = std::clamp(strokeWidth, 1.0, 72.0);
    commitArrowProperty(SnowCanvasShapeStylePropertyStrokeWidth,
                        [clampedStrokeWidth](SnowCanvasArrowStyle& style) {
                            if (qFuzzyCompare(style.strokeWidth + 1.0, clampedStrokeWidth + 1.0)) {
                                return false;
                            }
                            style.strokeWidth = clampedStrokeWidth;
                            return true;
                        });
}

void ScreenshotToolPaletteStyleControls::cycleArrowStrokeWidth() {
    const QVector<double>& values = style_presets::strokePresetWidths();
    for (double value : values) {
        if (value > m_state.m_arrowStyle.strokeWidth) {
            setArrowStrokeWidth(value);
            return;
        }
    }

    if (!values.isEmpty()) {
        setArrowStrokeWidth(values.first());
    }
}

void ScreenshotToolPaletteStyleControls::setArrowStrokeColor(const QColor& color) {
    commitArrowProperty(SnowCanvasShapeStylePropertyStrokeColor,
                        [color](SnowCanvasArrowStyle& style) {
                            if (!color.isValid() || style.stroke == color) {
                                return false;
                            }
                            style.stroke = color;
                            style.stroke.setAlpha(255);
                            return true;
                        });
}

void ScreenshotToolPaletteStyleControls::setArrowStrokeStyle(SnowCanvasStrokeStyle strokeStyle) {
    commitArrowProperty(SnowCanvasShapeStylePropertyStrokeStyle,
                        [strokeStyle](SnowCanvasArrowStyle& style) {
                            if (style.strokeStyle == strokeStyle) {
                                return false;
                            }
                            style.strokeStyle = strokeStyle;
                            return true;
                        });
}

void ScreenshotToolPaletteStyleControls::setArrowType(SnowCanvasArrowType arrowType) {
    commitArrowProperty(SnowCanvasShapeStylePropertyArrowType,
                        [arrowType](SnowCanvasArrowStyle& style) {
                            if (style.arrowType == arrowType) {
                                return false;
                            }
                            style.arrowType = arrowType;
                            return true;
                        });
}

void ScreenshotToolPaletteStyleControls::setArrowhead(bool start, SnowCanvasArrowhead arrowhead) {
    const quint32 property = start ? SnowCanvasShapeStylePropertyStartArrowhead
                                   : SnowCanvasShapeStylePropertyEndArrowhead;
    commitArrowProperty(property, [start, arrowhead](SnowCanvasArrowStyle& style) {
        SnowCanvasArrowhead& currentArrowhead = start ? style.startArrowhead : style.endArrowhead;
        if (currentArrowhead == arrowhead) {
            return false;
        }
        currentArrowhead = arrowhead;
        return true;
    });
}

void ScreenshotToolPaletteStyleControls::setTextColor(const QColor& color) {
    commitTextProperty(
        SnowCanvasTextStyleMixedColor,
        [color](ScreenshotToolPaletteTextStyleModel& style) { return style.setColor(color); },
        [color](const ScreenshotToolPaletteTextStyleModel&,
                ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setColor(color));
        });
}

void ScreenshotToolPaletteStyleControls::setTextFontSize(double fontSize) {
    commitTextProperty(
        SnowCanvasTextStyleMixedFontSize,
        [fontSize](ScreenshotToolPaletteTextStyleModel& style) {
            return style.setFontSize(fontSize);
        },
        [](const ScreenshotToolPaletteTextStyleModel& style,
           ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setFontSize(style.textStyle().fontSize));
        });
}

void ScreenshotToolPaletteStyleControls::cycleTextFontSize() {
    if (m_state.m_textStyle.cycleFontSize()) {
        static_cast<void>(
            m_state.m_creationTextStyle.setFontSize(m_state.m_textStyle.textStyle().fontSize));
        m_state.m_textStyleMixed &= ~SnowCanvasTextStyleMixedFontSize;
        updateTextStyleControls();
        notifyTextStyleChanged();
    }
}

void ScreenshotToolPaletteStyleControls::setTextFontFamily(const QString& fontFamily) {
    commitTextProperty(
        SnowCanvasTextStyleMixedFontFamily,
        [fontFamily](ScreenshotToolPaletteTextStyleModel& style) {
            return style.setFontFamily(fontFamily);
        },
        [fontFamily](const ScreenshotToolPaletteTextStyleModel&,
                     ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setFontFamily(fontFamily));
        });
}

void ScreenshotToolPaletteStyleControls::setTextStrokeColor(const QColor& color) {
    commitTextProperty(
        SnowCanvasTextStyleMixedStroke,
        [color](ScreenshotToolPaletteTextStyleModel& style) { return style.setStrokeColor(color); },
        [color](const ScreenshotToolPaletteTextStyleModel&,
                ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setStrokeColor(color));
        });
}

void ScreenshotToolPaletteStyleControls::setTextStrokeWidth(double strokeWidth) {
    commitTextProperty(
        SnowCanvasTextStyleMixedStrokeWidth,
        [strokeWidth](ScreenshotToolPaletteTextStyleModel& style) {
            return style.setStrokeWidth(strokeWidth);
        },
        [](const ScreenshotToolPaletteTextStyleModel& style,
           ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setStrokeWidth(style.textStyle().strokeWidth));
        });
}

void ScreenshotToolPaletteStyleControls::setTextFillColor(const QColor& color) {
    commitTextProperty(
        SnowCanvasTextStyleMixedFill,
        [color](ScreenshotToolPaletteTextStyleModel& style) { return style.setFillColor(color); },
        [color](const ScreenshotToolPaletteTextStyleModel&,
                ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setFillColor(color));
        });
}

void ScreenshotToolPaletteStyleControls::setTextFillStyle(SnowCanvasFillStyle fillStyle) {
    commitTextProperty(
        SnowCanvasTextStyleMixedFillStyle,
        [fillStyle](ScreenshotToolPaletteTextStyleModel& style) {
            return style.setFillStyle(fillStyle);
        },
        [fillStyle](const ScreenshotToolPaletteTextStyleModel&,
                    ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setFillStyle(fillStyle));
        });
}

void ScreenshotToolPaletteStyleControls::setTextCornerRadius(int cornerRadius) {
    commitTextProperty(
        SnowCanvasTextStyleMixedCornerRadii,
        [cornerRadius](ScreenshotToolPaletteTextStyleModel& style) {
            return style.setCornerRadius(cornerRadius);
        },
        [cornerRadius](const ScreenshotToolPaletteTextStyleModel&,
                       ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setCornerRadius(cornerRadius));
        });
}

void ScreenshotToolPaletteStyleControls::setTextHorizontalAlign(
    SnowCanvasTextHorizontalAlign alignment) {
    commitTextProperty(
        SnowCanvasTextStyleMixedHorizontalAlign,
        [alignment](ScreenshotToolPaletteTextStyleModel& style) {
            return style.setHorizontalAlign(alignment);
        },
        [alignment](const ScreenshotToolPaletteTextStyleModel&,
                    ScreenshotToolPaletteTextStyleModel& creation) {
            static_cast<void>(creation.setHorizontalAlign(alignment));
        });
}

void ScreenshotToolPaletteStyleControls::setWatermarkColor(const QColor& color) {
    if (!color.isValid() ||
        (m_state.m_watermarkConfig.color == color && !m_watermarkColorPreviewPending)) {
        return;
    }
    m_state.m_watermarkConfig.color = color;
    m_watermarkColorPreviewPending = false;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::setWatermarkFontSize(double fontSize) {
    if (!std::isfinite(fontSize)) {
        return;
    }
    commitWatermarkField([fontSize](SnowCanvasWatermarkConfig& config) {
        const double clamped = std::clamp(fontSize, kMinWatermarkFontSize, kMaxWatermarkFontSize);
        if (qFuzzyCompare(config.fontSize + 1.0, clamped + 1.0)) {
            return false;
        }
        config.fontSize = clamped;
        return true;
    });
}

void ScreenshotToolPaletteStyleControls::cycleWatermarkFontSize() {
    const QVector<double>& values = style_presets::watermarkFontSizes();
    for (double value : values) {
        if (value > m_state.m_watermarkConfig.fontSize + 0.001) {
            setWatermarkFontSize(value);
            return;
        }
    }
    if (!values.isEmpty()) {
        setWatermarkFontSize(values.first());
    }
}

void ScreenshotToolPaletteStyleControls::setWatermarkFontFamily(const QString& fontFamily) {
    const QString normalized = fontFamily.trimmed();
    commitWatermarkField([&normalized](SnowCanvasWatermarkConfig& config) {
        if (config.fontFamily == normalized) {
            return false;
        }
        config.fontFamily = normalized;
        return true;
    });
}

void ScreenshotToolPaletteStyleControls::setWatermarkAngle(double angle) {
    if (!std::isfinite(angle)) {
        return;
    }
    commitWatermarkField([angle](SnowCanvasWatermarkConfig& config) {
        const double clamped = std::clamp(angle, -90.0, 90.0);
        if (qFuzzyCompare(config.angle + 1.0, clamped + 1.0)) {
            return false;
        }
        config.angle = clamped;
        return true;
    });
}

void ScreenshotToolPaletteStyleControls::setWatermarkGap(double gap) {
    if (!std::isfinite(gap)) {
        return;
    }
    commitWatermarkField([gap](SnowCanvasWatermarkConfig& config) {
        const double clamped = std::clamp(gap, 10.0, 200.0);
        if (qFuzzyCompare(config.gap + 1.0, clamped + 1.0)) {
            return false;
        }
        config.gap = clamped;
        return true;
    });
}

void ScreenshotToolPaletteStyleControls::setWatermarkOpacity(double opacity) {
    if (!std::isfinite(opacity)) {
        return;
    }
    commitWatermarkField([opacity](SnowCanvasWatermarkConfig& config) {
        const double clamped = std::clamp(opacity, 0.0, 1.0);
        if (qFuzzyCompare(config.opacity + 1.0, clamped + 1.0)) {
            return false;
        }
        config.opacity = clamped;
        return true;
    });
}

void ScreenshotToolPaletteStyleControls::setSerialNumberColor(const QColor& color) {
    commitSerialNumberProperty(
        SnowCanvasSerialNumberStyleMixedColor,
        [color](SnowCanvasSerialNumberStyle& style, SnowCanvasSerialNumberStyle& creation) {
            if (!color.isValid() || style.color == color) {
                return false;
            }
            style.color = color;
            creation.color = color;
            return true;
        });
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFillColor(const QColor& color) {
    commitSerialNumberProperty(
        SnowCanvasSerialNumberStyleMixedFill,
        [color](SnowCanvasSerialNumberStyle& style, SnowCanvasSerialNumberStyle& creation) {
            if (!color.isValid() || style.fill == color) {
                return false;
            }
            style.fill = color;
            creation.fill = color;
            return true;
        });
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFillStyle(SnowCanvasFillStyle fillStyle) {
    commitSerialNumberProperty(
        SnowCanvasSerialNumberStyleMixedFillStyle,
        [fillStyle](SnowCanvasSerialNumberStyle& style, SnowCanvasSerialNumberStyle& creation) {
            if (style.fillStyle == fillStyle) {
                return false;
            }
            style.fillStyle = fillStyle;
            creation.fillStyle = fillStyle;
            return true;
        });
}

void ScreenshotToolPaletteStyleControls::setSerialNumber(qint64 number) {
    number = std::max<qint64>(0, number);
    commitSerialNumberProperty(
        SnowCanvasSerialNumberStyleMixedNumber,
        [number](SnowCanvasSerialNumberStyle& style, SnowCanvasSerialNumberStyle& creation) {
            if (style.number == number) {
                return false;
            }
            style.number = number;
            creation.number = number;
            return true;
        });
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFontSize(double fontSize) {
    const double clamped = std::clamp(fontSize, 6.0, 512.0);
    commitSerialNumberProperty(
        SnowCanvasSerialNumberStyleMixedFontSize,
        [clamped](SnowCanvasSerialNumberStyle& style, SnowCanvasSerialNumberStyle& creation) {
            if (qFuzzyCompare(style.fontSize + 1.0, clamped + 1.0)) {
                return false;
            }
            style.fontSize = clamped;
            creation.fontSize = clamped;
            return true;
        });
}

void ScreenshotToolPaletteStyleControls::cycleSerialNumberFontSize() {
    const QVector<double>& values = m_state.m_textStyle.fontSizeValues();
    for (double value : values) {
        if (value > m_state.m_serialNumberStyle.fontSize) {
            setSerialNumberFontSize(value);
            return;
        }
    }
    if (!values.isEmpty()) {
        setSerialNumberFontSize(values.first());
    }
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFontFamily(const QString& fontFamily) {
    const QString normalized = fontFamily.trimmed();
    commitSerialNumberProperty(
        SnowCanvasSerialNumberStyleMixedFontFamily,
        [&normalized](SnowCanvasSerialNumberStyle& style, SnowCanvasSerialNumberStyle& creation) {
            if (style.fontFamily == normalized) {
                return false;
            }
            style.fontFamily = normalized;
            creation.fontFamily = normalized;
            return true;
        });
}

void ScreenshotToolPaletteStyleControls::setStyleToolbarState(
    const SnowCanvasStyleToolbarState& state) {
    const auto editorInteracting = [](const auto& editor) {
        return editor != nullptr && editor->isInteracting();
    };
    const bool serialNumberStyleSource =
        state.source == SnowCanvasStyleToolbarSource::DefaultSerialNumber ||
        state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;
    if (serialNumberStyleSource) {
        const bool selectedSerialNumber =
            state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;
        SnowCanvasSerialNumberStyle displayedStyle = state.serialNumberStyle;
        if (editorInteracting(m_serialNumberColorEditor)) {
            displayedStyle.color = m_state.m_serialNumberStyle.color;
        }
        if (editorInteracting(m_serialNumberFillEditor)) {
            displayedStyle.fill = m_state.m_serialNumberStyle.fill;
        }
        const quint32 mixed = selectedSerialNumber ? state.serialNumberStyleMixed : 0;
        if (m_state.m_styleSource == state.source &&
            m_state.m_serialNumberStyle == displayedStyle &&
            m_state.m_serialNumberStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_state.m_serialNumberStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_state.m_styleSource != state.source) {
            groups = AllSerialNumberRefreshes;
        } else {
            if (m_state.m_serialNumberStyle.number != displayedStyle.number ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedNumber) != 0)
                groups |= SerialNumberValueRefresh;
            if (m_state.m_serialNumberStyle.color != displayedStyle.color ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedColor) != 0)
                groups |= SerialNumberColorRefresh;
            if (m_state.m_serialNumberStyle.fill != displayedStyle.fill ||
                m_state.m_serialNumberStyle.fillStyle != displayedStyle.fillStyle ||
                (mixedChanged & (SnowCanvasSerialNumberStyleMixedFill |
                                 SnowCanvasSerialNumberStyleMixedFillStyle)) != 0)
                groups |= SerialNumberFillRefresh;
            if (m_state.m_serialNumberStyle.fontSize != displayedStyle.fontSize ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedFontSize) != 0)
                groups |= SerialNumberFontSizeRefresh;
            if (m_state.m_serialNumberStyle.fontFamily != displayedStyle.fontFamily ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedFontFamily) != 0)
                groups |= SerialNumberFontFamilyRefresh;
        }
        m_state.m_styleSource = state.source;
        m_state.m_serialNumberStyleMixed = mixed;
        m_state.m_serialNumberStyle = displayedStyle;
        if (!selectedSerialNumber) {
            m_state.m_creationSerialNumberStyle = displayedStyle;
        }
        updateSerialNumberStyleControls(groups);
        return;
    }

    const bool textStyleSource = state.source == SnowCanvasStyleToolbarSource::DefaultText ||
                                 state.source == SnowCanvasStyleToolbarSource::SelectedText;
    if (textStyleSource) {
        const bool selectedText = state.source == SnowCanvasStyleToolbarSource::SelectedText;
        SnowCanvasTextStyle displayedStyle = state.textStyle;
        if (editorInteracting(m_textColorEditor)) {
            displayedStyle.color = m_state.m_textStyle.textStyle().color;
        }
        if (editorInteracting(m_textStrokeEditor)) {
            displayedStyle.stroke = m_state.m_textStyle.textStyle().stroke;
        }
        if (editorInteracting(m_textFillEditor)) {
            displayedStyle.fill = m_state.m_textStyle.textStyle().fill;
        }
        const quint32 mixed = selectedText ? state.textStyleMixed : 0;
        const SnowCanvasTextStyle previous = m_state.m_textStyle.textStyle();
        m_state.m_textStyle.setTextStyle(displayedStyle);
        const SnowCanvasTextStyle normalized = m_state.m_textStyle.textStyle();
        if (m_state.m_styleSource == state.source && previous == normalized &&
            m_state.m_textStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_state.m_textStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_state.m_styleSource != state.source) {
            groups = AllTextRefreshes;
        } else {
            if (previous.color != normalized.color ||
                (mixedChanged & SnowCanvasTextStyleMixedColor) != 0)
                groups |= TextColorRefresh;
            if (previous.fontSize != normalized.fontSize ||
                (mixedChanged & SnowCanvasTextStyleMixedFontSize) != 0)
                groups |= TextFontSizeRefresh;
            if (previous.fontFamily != normalized.fontFamily ||
                (mixedChanged & SnowCanvasTextStyleMixedFontFamily) != 0)
                groups |= TextFontFamilyRefresh;
            if (previous.stroke != normalized.stroke ||
                previous.strokeWidth != normalized.strokeWidth ||
                (mixedChanged &
                 (SnowCanvasTextStyleMixedStroke | SnowCanvasTextStyleMixedStrokeWidth)) != 0)
                groups |= TextStrokeRefresh;
            if (previous.fill != normalized.fill || previous.fillStyle != normalized.fillStyle ||
                (mixedChanged &
                 (SnowCanvasTextStyleMixedFill | SnowCanvasTextStyleMixedFillStyle)) != 0)
                groups |= TextFillRefresh;
            if (previous.cornerRadii != normalized.cornerRadii ||
                (mixedChanged & SnowCanvasTextStyleMixedCornerRadii) != 0)
                groups |= TextCornerRefresh;
            if (previous.horizontalAlign != normalized.horizontalAlign ||
                previous.verticalAlign != normalized.verticalAlign ||
                (mixedChanged & (SnowCanvasTextStyleMixedHorizontalAlign |
                                 SnowCanvasTextStyleMixedVerticalAlign)) != 0)
                groups |= TextAlignmentRefresh;
        }
        m_state.m_styleSource = state.source;
        m_state.m_showingSelectedTextStyle = selectedText;
        m_state.m_textStyleMixed = mixed;
        if (!selectedText) {
            m_state.m_creationTextStyle.setTextStyle(normalized);
        }
        updateTextStyleControls(groups);
        return;
    }

    const bool arrowStyleSource = state.source == SnowCanvasStyleToolbarSource::DefaultArrow ||
                                  state.source == SnowCanvasStyleToolbarSource::SelectedArrow;
    if (arrowStyleSource) {
        const bool selectedArrow = state.source == SnowCanvasStyleToolbarSource::SelectedArrow;
        SnowCanvasArrowStyle displayedStyle = arrowStyleFromShapeStyle(state.shapeStyle);
        if (editorInteracting(m_arrowStrokeEditor)) {
            displayedStyle.stroke = m_state.m_arrowStyle.stroke;
        }
        const quint32 mixed = selectedArrow ? state.shapeStyleMixed : 0;
        if (m_state.m_styleSource == state.source && m_state.m_arrowStyle == displayedStyle &&
            m_state.m_selectedStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_state.m_selectedStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_state.m_styleSource != state.source) {
            groups = AllShapeRefreshes;
        } else {
            if (m_state.m_arrowStyle.strokeWidth != displayedStyle.strokeWidth ||
                (mixedChanged & SnowCanvasShapeStyleMixedStrokeWidth) != 0)
                groups |= ShapeStrokeWidthRefresh;
            if (m_state.m_arrowStyle.stroke != displayedStyle.stroke ||
                m_state.m_arrowStyle.strokeStyle != displayedStyle.strokeStyle ||
                (mixedChanged &
                 (SnowCanvasShapeStyleMixedStroke | SnowCanvasShapeStyleMixedStrokeStyle)) != 0)
                groups |= ShapeStrokeRefresh;
            if (m_state.m_arrowStyle.arrowType != displayedStyle.arrowType ||
                (mixedChanged & SnowCanvasShapeStyleMixedArrowType) != 0)
                groups |= ShapeArrowTypeRefresh;
            if (m_state.m_arrowStyle.startArrowhead != displayedStyle.startArrowhead ||
                m_state.m_arrowStyle.endArrowhead != displayedStyle.endArrowhead ||
                (mixedChanged & (SnowCanvasShapeStyleMixedStartArrowhead |
                                 SnowCanvasShapeStyleMixedEndArrowhead)) != 0)
                groups |= ShapeArrowheadsRefresh;
        }
        m_state.m_styleSource = state.source;
        m_state.m_showingSelectedStyle = selectedArrow;
        m_state.m_selectedStyleMixed = mixed;
        m_state.m_arrowStyle = displayedStyle;
        if (!selectedArrow) {
            m_state.m_creationArrowStyle = displayedStyle;
        }
        updateArrowStyleControls(groups);
        return;
    }

    const bool penHighlightStyleSource =
        state.source == SnowCanvasStyleToolbarSource::DefaultPenHighlight ||
        state.source == SnowCanvasStyleToolbarSource::SelectedPenHighlight;
    if (penHighlightStyleSource) {
        const bool selectedPenHighlight =
            state.source == SnowCanvasStyleToolbarSource::SelectedPenHighlight;
        SnowCanvasShapeStyle displayedStyle = state.shapeStyle;
        if (editorInteracting(m_penHighlightColorEditor)) {
            displayedStyle.stroke = m_state.m_penHighlightStyle.stroke;
        }
        const quint32 mixed = selectedPenHighlight ? state.shapeStyleMixed : 0;
        if (m_state.m_styleSource == state.source &&
            m_state.m_penHighlightStyle == displayedStyle &&
            m_state.m_selectedStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_state.m_selectedStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_state.m_styleSource != state.source)
            groups = AllShapeRefreshes;
        else {
            if (m_state.m_penHighlightStyle.stroke != displayedStyle.stroke ||
                (mixedChanged & SnowCanvasShapeStyleMixedStroke) != 0)
                groups |= ShapeStrokeRefresh;
            if (m_state.m_penHighlightStyle.strokeWidth != displayedStyle.strokeWidth ||
                (mixedChanged & SnowCanvasShapeStyleMixedStrokeWidth) != 0)
                groups |= ShapeStrokeWidthRefresh;
        }
        m_state.m_styleSource = state.source;
        m_state.m_showingSelectedStyle = selectedPenHighlight;
        m_state.m_selectedStyleMixed = mixed;
        m_state.m_penHighlightStyle = displayedStyle;
        if (!selectedPenHighlight) {
            m_state.m_creationPenHighlightStyle = displayedStyle;
        }
        updatePenHighlightStyleControls(groups);
        return;
    }

    const bool lineStyleSource = state.source == SnowCanvasStyleToolbarSource::DefaultLine ||
                                 state.source == SnowCanvasStyleToolbarSource::SelectedLine;
    const bool freeDrawStyleSource =
        state.source == SnowCanvasStyleToolbarSource::DefaultFreeDraw ||
        state.source == SnowCanvasStyleToolbarSource::SelectedFreeDraw;
    const bool highlightStyleSource =
        state.source == SnowCanvasStyleToolbarSource::DefaultRectangleHighlight ||
        state.source == SnowCanvasStyleToolbarSource::SelectedRectangleHighlight;
    const bool rectangleStyleSource =
        state.source == SnowCanvasStyleToolbarSource::DefaultRectangle ||
        state.source == SnowCanvasStyleToolbarSource::SelectedRectangle;
    if (!rectangleStyleSource && !lineStyleSource && !freeDrawStyleSource &&
        !highlightStyleSource) {
        return;
    }
    const bool selectedShape =
        state.source == SnowCanvasStyleToolbarSource::SelectedRectangle ||
        state.source == SnowCanvasStyleToolbarSource::SelectedLine ||
        state.source == SnowCanvasStyleToolbarSource::SelectedFreeDraw ||
        state.source == SnowCanvasStyleToolbarSource::SelectedRectangleHighlight;
    auto& displayedModel = highlightStyleSource  ? m_state.m_highlightStyle
                           : freeDrawStyleSource ? m_state.m_freeDrawStyle
                           : lineStyleSource     ? m_state.m_lineStyle
                                                 : m_state.m_rectangleStyle;
    auto& creationModel = highlightStyleSource  ? m_state.m_creationHighlightStyle
                          : freeDrawStyleSource ? m_state.m_creationFreeDrawStyle
                          : lineStyleSource     ? m_state.m_creationLineStyle
                                                : m_state.m_creationRectangleStyle;
    SnowCanvasShapeStyle displayedStyle = state.shapeStyle;
    if (editorInteracting(m_shapeStrokeEditor)) {
        displayedStyle.stroke = displayedModel.strokeColor();
    }
    if (highlightStyleSource && editorInteracting(m_highlightColorEditor)) {
        displayedStyle.fill = displayedModel.fillColor();
    }
    if (highlightStyleSource && editorInteracting(m_highlightStrokeEditor)) {
        displayedStyle.stroke = displayedModel.strokeColor();
    }
    const quint32 mixed = selectedShape ? state.shapeStyleMixed : 0;
    if (m_state.m_styleSource == state.source &&
        displayedModel.rectangleStyle() == displayedStyle &&
        m_state.m_selectedStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_styleStateNoopCount;
#endif
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
        return;
    }
    m_state.m_lineControlsActive = lineStyleSource;
    m_state.m_freeDrawControlsActive = freeDrawStyleSource;
    m_state.m_highlightControlsActive = highlightStyleSource;
    m_state.m_showingSelectedStyle = selectedShape;
    updateRectangleOnlyControlsVisibility();
    const SnowCanvasShapeStyle previous = displayedModel.rectangleStyle();
    const quint32 mixedChanged = m_state.m_selectedStyleMixed ^ mixed;
    quint32 groups = 0;
    if (m_state.m_styleSource != state.source)
        groups = AllShapeRefreshes;
    else {
        if (previous.shape != displayedStyle.shape ||
            previous.highlightShape != displayedStyle.highlightShape ||
            (mixedChanged &
             (SnowCanvasShapeStyleMixedShape | SnowCanvasShapeStyleMixedHighlightShape)) != 0)
            groups |= ShapeModeRefresh;
        if (previous.strokeWidth != displayedStyle.strokeWidth ||
            (mixedChanged & SnowCanvasShapeStyleMixedStrokeWidth) != 0)
            groups |= ShapeStrokeWidthRefresh;
        if (previous.stroke != displayedStyle.stroke ||
            previous.strokeStyle != displayedStyle.strokeStyle ||
            (mixedChanged &
             (SnowCanvasShapeStyleMixedStroke | SnowCanvasShapeStyleMixedStrokeStyle)) != 0)
            groups |= ShapeStrokeRefresh;
        if (previous.fill != displayedStyle.fill ||
            previous.fillStyle != displayedStyle.fillStyle ||
            (mixedChanged & (SnowCanvasShapeStyleMixedFill | SnowCanvasShapeStyleMixedFillStyle)) !=
                0)
            groups |= ShapeFillRefresh;
        if (previous.cornerRadii != displayedStyle.cornerRadii ||
            (mixedChanged & SnowCanvasShapeStyleMixedCornerRadii) != 0)
            groups |= ShapeCornerRefresh;
    }
    m_state.m_styleSource = state.source;
    if (m_state.m_showingSelectedStyle) {
        displayedModel.setRectangleStyle(displayedStyle);
        m_state.m_selectedStyleMixed = mixed;
    } else {
        creationModel.setRectangleStyle(displayedStyle);
        displayedModel.setRectangleStyle(displayedStyle);
        m_state.m_selectedStyleMixed = 0;
    }
    updateRectangleStyleControls(groups);
}
