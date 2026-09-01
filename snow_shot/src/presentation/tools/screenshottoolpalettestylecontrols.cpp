#include "screenshottoolpalettestylecontrols.h"
#include "screenshottoolbarperfinstrumentation.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "antd_icons.h"
#include "widgets/color_picker.h"
#include "widgets/control_scale.h"
#include "widgets/input_line_edit.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/radio_button_group.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QListView>
#include <QObject>
#include <QScopedValueRollback>
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
constexpr int kColorPickerOptionSpacing = 4;
constexpr int kCornerRadiusLeadingSpacing = 4;
constexpr int kTextStrokeColorTrailingSpacing = 4;
constexpr int kSerialNumberTrailingSpacing = 4;
constexpr int kOpacitySliderWidth = 96;
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
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark Text"),
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
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "%1 (Unavailable)"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight stroke width"),
};

[[maybe_unused]] constexpr const char* kStartArrowheadOptionTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead None"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Standard"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Bar"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Dot"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Circle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Circle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Triangle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Triangle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Diamond"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Diamond outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Crowfoot one"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Crowfoot many"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start arrowhead Crowfoot one or many"),
};

[[maybe_unused]] constexpr const char* kEndArrowheadOptionTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead None"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Standard"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Bar"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Dot"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Circle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Circle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Triangle"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Triangle outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Diamond"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Diamond outline"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Crowfoot one"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Crowfoot many"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "End arrowhead Crowfoot one or many"),
};

int defaultCornerRadius() {
    return qRound(
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.cornerRadii.topLeft);
}

void configureStylePopupTrigger(QWidget* trigger, const QString& source) {
    if (trigger == nullptr) {
        return;
    }

    const QByteArray sourceUtf8 = source.toUtf8();
    setScreenshotToolPaletteAccessibleNameSource(trigger, sourceUtf8.constData());
    trigger->setToolTip(QString());
    trigger->setAccessibleName(ScreenshotToolPaletteTranslationText(source).translated());
}

const QVector<double>& arrowStrokeWidthValues() {
    static const QVector<double> values{2.0, 4.0, 8.0};
    return values;
}

const QVector<double>& watermarkFontSizeValues() {
    static const QVector<double> values{12.0, 16.0, 24.0, 30.0};
    return values;
}

const QVector<QColor>& arrowStrokeColorValues() {
    static const QVector<QColor> values{
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle.stroke,
        QColor(QStringLiteral("#52c41a")),
        QColor(QStringLiteral("#1677ff")),
        QColor(QStringLiteral("#fadb14")),
        QColor(QStringLiteral("#000000")),
    };
    return values;
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

adqt::icons::IconRef fontSizePresetIcon(int index) {
    switch (index) {
    case 0:
        return custom_outlined_icons::FontSizeSmall();
    case 1:
        return custom_outlined_icons::FontSizeMedium();
    case 2:
        return custom_outlined_icons::FontSizeLarge();
    case 3:
        return custom_outlined_icons::FontSizeVeryLarge();
    default:
        return custom_outlined_icons::FontSizeMedium();
    }
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

void setButtonActive(adqt::widgets::AdButton* button, bool active) {
    if (button == nullptr) {
        return;
    }

    button->setButtonStyle(active ? adqt::widgets::AdButton::ButtonStyle::Tonal
                                  : adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(active ? adqt::widgets::AdButton::AccentRole::Primary
                                 : adqt::widgets::AdButton::AccentRole::Neutral);
}

void activateWidgetLayoutTree(QWidget* widget);

void configureColorPickerMetrics(adqt::widgets::AdColorPicker* picker,
                                 const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, picker)) {
        return;
    }

    const int buttonSize = qMax(1, qRound(metrics.buttonSize * metrics.physicalScale));
    picker->setFixedSize(buttonSize, buttonSize);
    activateWidgetLayoutTree(picker);
}

int colorPickerOptionSpacing(const ScreenshotToolPaletteButtonMetrics& metrics) {
    return qMax(0, qRound(kColorPickerOptionSpacing * metrics.physicalScale));
}

ScreenshotToolPaletteButtonMetrics
popupButtonMetrics(const ScreenshotToolPaletteButtonMetrics& toolbarMetrics) {
    ScreenshotToolPaletteButtonMetrics metrics = toolbarMetrics;
    // QtTool windows follow their monitor's DPR independently. Reusing the
    // toolbar counter-scale would leave popup content at the source DPI.
    metrics.physicalScale = 1.0;
    return metrics;
}

void activateLayoutTree(QLayout* layout) {
    if (layout == nullptr) {
        return;
    }

    // Color-picker triggers use nested host layouts; update their size hints
    // before the picker root layout positions the host after a DPI commit.
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item == nullptr) {
            continue;
        }
        if (QWidget* childWidget = item->widget()) {
            activateWidgetLayoutTree(childWidget);
        } else if (QLayout* childLayout = item->layout()) {
            activateLayoutTree(childLayout);
        }
    }
    layout->invalidate();
    layout->activate();
}

void activateWidgetLayoutTree(QWidget* widget) {
    if (widget != nullptr && widget->layout() != nullptr) {
        activateLayoutTree(widget->layout());
    }
}

void resetPopupButtonControlScale(adqt::widgets::AdButton* button) {
    if (button == nullptr) {
        return;
    }

    const adqt::widgets::AdControlScaleContext popupContext =
        adqt::widgets::AdControlScaleContext::fromDprs(1.0, 1.0);
    button->prepareControlScale(popupContext);
    button->commitControlScale(popupContext);
}

} // namespace

ScreenshotToolPaletteStyleControls::ScreenshotToolPaletteStyleControls(
    ScreenshotToolPaletteStyleControlCallbacks callbacks, const SnowCanvasStyleDefaults& defaults)
    : ScreenshotToolPaletteStyleState(defaults), m_callbacks(std::move(callbacks)),
      m_defaults(defaults) {}

ScreenshotToolPaletteStyleState& ScreenshotToolPaletteStyleControls::styleState() {
    return *this;
}

const ScreenshotToolPaletteStyleState& ScreenshotToolPaletteStyleControls::styleState() const {
    return *this;
}

ScreenshotToolPaletteStyleControls::FontEditor ScreenshotToolPaletteStyleControls::addFontEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const FontEditorConfig& config,
    double initialSize, const QString& initialFamily, const std::function<void()>& cycleSize,
    const std::function<void(double)>& setSize,
    const std::function<void(const QString&)>& setFamily,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    FontEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return editor;
    }

    editor.sizeValues = config.sizeValues;
    editor.sizeSummary = createScreenshotToolPaletteNumericValueButton(
        parent, config.summaryTooltip.toUtf8().constData(), initialSize, QStringLiteral("px"),
        metrics);
    if (!config.summaryObjectName.isEmpty()) {
        editor.sizeSummary->setObjectName(config.summaryObjectName);
    }
    layout->addWidget(editor.sizeSummary);
    QObject::connect(editor.sizeSummary, &adqt::widgets::AdButton::clicked, receiver, cycleSize);

    for (int index = 0; index < editor.sizeValues.size(); ++index) {
        const double value = editor.sizeValues.at(index);
        const ScreenshotToolPaletteTranslationText tooltip =
            config.presetTooltip ? config.presetTooltip(index, value)
                                 : ScreenshotToolPaletteTranslationText(QString::number(value));
        auto* button = createScreenshotToolPaletteStyleActionButton(
            parent, nullptr, fontSizePresetIcon(index), metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        editor.sizePresets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setSize, value]() {
            if (setSize) {
                setSize(value);
            }
        });
    }

    ScreenshotToolPaletteSelectEditorConfig selectConfig;
    selectConfig.accessibleName = config.accessibleName;
    selectConfig.placeholder = QStringLiteral("Font family");
    selectConfig.searchEnabled = true;
    const ScreenshotToolPaletteSelectEditor selectEditor =
        createScreenshotToolPaletteSelectEditor(parent, selectConfig, metrics);
    editor.familySelect = selectEditor.select;
    if (config.observePopup) {
        observeTextStylePopup(editor.familySelect);
    }
    configureStylePopupTrigger(editor.familySelect, config.accessibleName);
    editor.familySelect->setSortComparator(
        [](const adqt::widgets::AdSelect::Option& lhs, const adqt::widgets::AdSelect::Option& rhs) {
            const bool lhsIsDefault = lhs.value.toString().isEmpty();
            const bool rhsIsDefault = rhs.value.toString().isEmpty();
            if (lhsIsDefault != rhsIsDefault) {
                return lhsIsDefault;
            }
            return QString::compare(lhs.label, rhs.label, Qt::CaseInsensitive) < 0;
        });

    auto* fontModel = new QStandardItemModel(editor.familySelect);
    const auto appendFont = [fontModel](const QString& label, const char* source,
                                        const QString& value, bool enabled, bool preview) {
        auto* item = new QStandardItem(label);
        if (source != nullptr) {
            setScreenshotToolPaletteItemTranslationSource(item, source);
        } else {
            item->setData(label, adqt::widgets::AdSelect::DefaultLabelRole);
        }
        item->setData(value, adqt::widgets::AdSelect::DefaultValueRole);
        item->setEnabled(enabled);
        if (preview && !value.isEmpty()) {
            item->setData(QFont(value), Qt::FontRole);
        }
        fontModel->appendRow(item);
    };
    appendFont(QStringLiteral("Default"), "Default", QString(), true, false);
    appendFont(QStringLiteral("Mixed"), "Mixed", QStringLiteral("__mixed__"), false, false);
    QStringList fontFamilies = QFontDatabase::families();
    fontFamilies.removeDuplicates();
    fontFamilies.sort(Qt::CaseInsensitive);
    for (const QString& family : std::as_const(fontFamilies)) {
        const QString trimmed = family.trimmed();
        if (!trimmed.isEmpty()) {
            appendFont(trimmed, nullptr, trimmed, true, true);
        }
    }
    editor.familySelect->setModel(fontModel);
    editor.familySelect->setCurrentData(initialFamily, adqt::widgets::AdSelect::DefaultValueRole);
    layout->addWidget(editor.familySelect);
    QObject::connect(editor.familySelect, &adqt::widgets::AdSelect::selected, receiver,
                     [setFamily](const QVariant& value, const QString&) {
                         if (setFamily && value.toString() != QStringLiteral("__mixed__")) {
                             setFamily(value.toString());
                         }
                     });
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateFontEditor(FontEditor& editor, double size,
                                                          const QString& family, bool sizeMixed,
                                                          bool familyMixed, quint32 groups,
                                                          quint32 sizeGroup, quint32 familyGroup) {
    if ((groups & sizeGroup) != 0) {
        if (editor.sizeSummary != nullptr) {
            editor.sizeSummary->setValue(size);
            editor.sizeSummary->setMixed(sizeMixed);
        }
        for (int index = 0; index < editor.sizePresets.size() && index < editor.sizeValues.size();
             ++index) {
            setButtonActive(editor.sizePresets.at(index),
                            !sizeMixed &&
                                qFuzzyCompare(editor.sizeValues.at(index) + 1.0, size + 1.0));
        }
    }

    if ((groups & familyGroup) == 0 || editor.familySelect == nullptr) {
        return;
    }

    const QSignalBlocker blocker(editor.familySelect);
    auto* model = qobject_cast<QStandardItemModel*>(editor.familySelect->model());
    if (!familyMixed && model != nullptr && !family.isEmpty()) {
        bool found = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->index(row, 0).data(adqt::widgets::AdSelect::DefaultValueRole).toString() ==
                family) {
                found = true;
                break;
            }
        }
        if (!found) {
            const ScreenshotToolPaletteTranslationText unavailableText =
                ScreenshotToolPaletteTranslationText("%1 (Unavailable)").arg(family);
            auto* item = new QStandardItem(unavailableText.translated());
            setScreenshotToolPaletteItemTranslationSource(item, unavailableText);
            item->setData(family, adqt::widgets::AdSelect::DefaultValueRole);
            item->setEnabled(false);
            model->appendRow(item);
        }
    }
    editor.familySelect->setCurrentData(familyMixed ? QVariant(QStringLiteral("__mixed__"))
                                                    : QVariant(family),
                                        adqt::widgets::AdSelect::DefaultValueRole);
}

void ScreenshotToolPaletteStyleControls::refreshFontEditorMetrics(
    FontEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    const auto applies = [&metrics](const QWidget* widget) {
        return screenshotToolPaletteMetricsApplyTo(metrics, widget);
    };
    if (applies(editor.sizeSummary)) {
        configureScreenshotToolPaletteStyleButton(editor.sizeSummary, nullptr, metrics);
        editor.sizeSummary->setPhysicalScale(metrics.physicalScale);
    }
    for (adqt::widgets::AdButton* button : editor.sizePresets) {
        if (applies(button)) {
            configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        }
    }
    ScreenshotToolPaletteSelectEditor selectEditor;
    selectEditor.select = editor.familySelect;
    configureScreenshotToolPaletteSelectEditor(selectEditor, metrics);
}

adqt::widgets::AdColorPicker* ScreenshotToolPaletteStyleControls::createColorPickerShell(
    QWidget* parent, const QString& accessibleName, const QColor& initialColor, bool alphaEnabled,
    bool observePopup) {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* picker = new adqt::widgets::AdColorPicker(parent);
    if (observePopup) {
        observeTextStylePopup(picker);
    }
    configureStylePopupTrigger(picker, accessibleName);
    picker->setFocusPolicy(Qt::NoFocus);
    picker->setSize(adqt::widgets::AdColorPicker::Size::Small);
    picker->setModeOptions({adqt::widgets::AdColorPicker::Mode::Solid});
    picker->setMode(adqt::widgets::AdColorPicker::Mode::Solid);
    picker->setTrigger(adqt::widgets::AdColorPicker::Trigger::Hover);
    picker->setTriggerTextVisible(false);
    picker->setAlphaChannelEnabled(alphaEnabled);
    picker->setAllowClear(false);
    picker->setPlacement(adqt::widgets::AdColorPicker::Placement::Bottom);
    picker->setPopupLayerMode(adqt::widgets::AdColorPicker::PopupLayerMode::QtTool);
    picker->setPopupContentPlacement(adqt::widgets::AdColorPicker::PopupContentPlacement::Top);
    picker->setValue(adqt::widgets::AdColorValue::solid(initialColor));
    auto* sampler = createScreenshotToolPaletteColorPickerSamplerButton(picker, initialColor);
    sampler->setObjectName(QStringLiteral("screenshot-color-picker-sampler"));
    configureScreenshotToolPaletteTooltip(
        sampler, ScreenshotToolPaletteTranslationText("Pick color from canvas"));
    picker->setPreviewContent(sampler);
    QObject::connect(picker, &adqt::widgets::AdColorPicker::valueChanged, sampler,
                     [sampler](const adqt::widgets::AdColorValue& value) {
                         if (value.isSolid() && value.solidColor.isValid()) {
                             sampler->setSwatchColor(value.solidColor);
                         }
                     });
    QObject::connect(sampler, &QAbstractButton::clicked, picker, [this, picker]() {
        picker->setPopupVisible(false);
        if (m_callbacks.canvasColorSamplingRequested) {
            m_callbacks.canvasColorSamplingRequested(picker);
        }
    });
    return picker;
}

void ScreenshotToolPaletteStyleControls::connectColorPickerChanges(
    adqt::widgets::AdColorPicker* picker, QObject* receiver, bool* handlingChange,
    const std::function<void(const QColor&)>& commitColor,
    const std::function<void(const QColor&)>& previewColor,
    const std::function<void(const QColor&)>& valueChanged) {
    if (picker == nullptr || receiver == nullptr) {
        return;
    }
    const auto dispatchColor = [handlingChange](const std::function<void(const QColor&)>& callback,
                                                const adqt::widgets::AdColorValue& value) {
        if (!callback || !value.isSolid() || !value.solidColor.isValid()) {
            return;
        }
        if (handlingChange != nullptr) {
            const QScopedValueRollback<bool> guard(*handlingChange, true);
            callback(value.solidColor);
            return;
        }
        callback(value.solidColor);
    };
    QObject::connect(
        picker, &adqt::widgets::AdColorPicker::valueChanged, receiver,
        [dispatchColor, valueChanged, callback = previewColor ? previewColor : commitColor](
            const adqt::widgets::AdColorValue& value) {
            if (valueChanged && value.isSolid() && value.solidColor.isValid()) {
                valueChanged(value.solidColor);
            }
            dispatchColor(callback, value);
        });
    if (previewColor) {
        QObject::connect(picker, &adqt::widgets::AdColorPicker::editingFinished, receiver,
                         [dispatchColor, commitColor](const adqt::widgets::AdColorValue& value) {
                             dispatchColor(commitColor, value);
                         });
    }
}

ScreenshotToolPaletteStyleControls::ColorPickerPopupLayout
ScreenshotToolPaletteStyleControls::createColorPickerPopupContent(
    adqt::widgets::AdColorPicker* picker, const QString& objectName,
    const ScreenshotToolPaletteButtonMetrics& popupMetrics) {
    ColorPickerPopupLayout content;
    if (picker == nullptr) {
        return content;
    }
    content.widget = new QWidget(picker);
    content.widget->setObjectName(objectName);
    content.layout = new QVBoxLayout(content.widget);
    content.layout->setContentsMargins(0, 0, 0, 0);
    content.layout->setSpacing(colorPickerOptionSpacing(popupMetrics));
    return content;
}

ScreenshotToolPaletteStyleControls::ColorPickerPopupLayout
ScreenshotToolPaletteStyleControls::addColorPickerPopupRow(
    ColorPickerPopupLayout& content, const QString& objectName,
    const ScreenshotToolPaletteButtonMetrics& popupMetrics) {
    ColorPickerPopupLayout row;
    if (content.widget == nullptr || content.layout == nullptr) {
        return row;
    }
    row.widget = new QWidget(content.widget);
    row.widget->setObjectName(objectName);
    row.layout = new QHBoxLayout(row.widget);
    row.layout->setContentsMargins(0, 0, 0, 0);
    row.layout->setSpacing(colorPickerOptionSpacing(popupMetrics));
    content.layout->addWidget(row.widget);
    return row;
}

ScreenshotToolPaletteStyleControls::IconOptionEditor
ScreenshotToolPaletteStyleControls::addIconOptionEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const IconOptionEditorConfig& config,
    int initialValue, const std::function<void(int)>& setValue,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    IconOptionEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr || config.options.isEmpty()) {
        return editor;
    }

    const auto initialOption = std::find_if(
        config.options.cbegin(), config.options.cend(),
        [initialValue](const IconOption& option) { return option.value == initialValue; });
    const adqt::icons::IconRef initialIcon = initialOption != config.options.cend()
                                                 ? initialOption->icon
                                                 : config.options.constFirst().icon;
    const QByteArray triggerTooltip = config.triggerTooltip.toUtf8();
    editor.trigger = createScreenshotToolPaletteIconValuePreviewTrigger(
        parent, triggerTooltip.constData(), initialIcon, metrics);
    configureStylePopupTrigger(editor.trigger, config.accessibleName);
    layout->addWidget(editor.trigger);

    editor.popover = new adqt::widgets::AdPopover(editor.trigger);
    editor.popover->setSourceWidget(editor.trigger);
    editor.popover->setTriggers(adqt::widgets::AdPopover::Trigger::Hover);
    editor.popover->setPlacement(adqt::widgets::AdPopover::Placement::Bottom);
    editor.popover->setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    editor.popover->setArrowVisible(true);
    editor.popover->setContentMargins(QMargins(8, 8, 8, 8));
    if (config.minimizeTitleWidth) {
        editor.popover->setTitleMinimumWidth(0);
    }

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    auto* content = new QWidget();
    QLayout* optionLayout = nullptr;
    QGridLayout* gridLayout = nullptr;
    if (config.gridColumnCount > 0) {
        gridLayout = new QGridLayout(content);
        optionLayout = gridLayout;
    } else {
        optionLayout = new QHBoxLayout(content);
    }
    optionLayout->setContentsMargins(0, 0, 0, 0);
    optionLayout->setSpacing(colorPickerOptionSpacing(popupMetrics));

    editor.options = config.options;
    for (int index = 0; index < editor.options.size(); ++index) {
        const IconOption& option = editor.options.at(index);
        auto* button = createScreenshotToolPaletteStyleActionButton(content, nullptr, option.icon,
                                                                    popupMetrics);
        configureScreenshotToolPaletteTooltip(button, option.tooltip);
        setButtonActive(button, option.value == initialValue);
        editor.buttons.push_back(button);
        if (gridLayout != nullptr) {
            gridLayout->addWidget(button, index / config.gridColumnCount,
                                  index % config.gridColumnCount);
        } else {
            static_cast<QHBoxLayout*>(optionLayout)->addWidget(button);
        }
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [popover = editor.popover, setValue, value = option.value]() {
                             setValue(value);
                             popover->hide();
                         });
    }
    editor.popover->setContentWidget(content);
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateIconOptionEditor(IconOptionEditor& editor, int value,
                                                                bool mixed) {
    const auto currentOption =
        std::find_if(editor.options.cbegin(), editor.options.cend(),
                     [value](const IconOption& option) { return option.value == value; });
    if (editor.trigger != nullptr) {
        if (currentOption != editor.options.cend()) {
            editor.trigger->setValueIconRef(currentOption->icon);
        }
        editor.trigger->setMixed(mixed);
    }
    for (int index = 0; index < editor.buttons.size() && index < editor.options.size(); ++index) {
        setButtonActive(editor.buttons.at(index),
                        !mixed && editor.options.at(index).value == value);
    }
}

void ScreenshotToolPaletteStyleControls::refreshIconOptionEditorMetrics(
    IconOptionEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (screenshotToolPaletteMetricsApplyTo(metrics, editor.trigger)) {
        configureScreenshotToolPaletteIconValuePreviewTrigger(editor.trigger, metrics);
    }
    for (adqt::widgets::AdButton* button : editor.buttons) {
        resetPopupButtonControlScale(button);
    }
}

ScreenshotToolPaletteStyleControls::ColorEditor ScreenshotToolPaletteStyleControls::addColorEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const ColorEditorConfig& config,
    const QColor& initialColor, bool* handlingChange,
    const std::function<void(const QColor&)>& commitColor,
    const std::function<void(const QColor&)>& previewColor,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    ColorEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return editor;
    }

    editor.presetValues = config.presetValues;
    editor.picker = createColorPickerShell(parent, config.accessibleName, initialColor,
                                           config.alphaEnabled, config.observePopup);
    if (!config.pickerObjectName.isEmpty()) {
        editor.picker->setObjectName(config.pickerObjectName);
    }
    editor.trigger = createScreenshotToolPaletteColorButton(
        editor.picker, config.accessibleName.toUtf8().constData(), initialColor, true, true,
        metrics);
    if (!config.triggerObjectName.isEmpty()) {
        editor.trigger->setObjectName(config.triggerObjectName);
    }
    configureStylePopupTrigger(editor.trigger, config.accessibleName);
    editor.picker->setTriggerContent(editor.trigger);
    configureColorPickerMetrics(editor.picker, metrics);
    layout->addWidget(editor.picker);

    connectColorPickerChanges(editor.picker, receiver, handlingChange, commitColor, previewColor,
                              [trigger = editor.trigger](const QColor& color) {
                                  if (trigger != nullptr) {
                                      trigger->setSwatchColor(color);
                                  }
                              });

    for (const QColor& color : editor.presetValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.presetTooltip ? config.presetTooltip(color)
                                 : ScreenshotToolPaletteTranslationText(color.name());
        auto* button =
            createScreenshotToolPaletteColorButton(parent, nullptr, color, false, true, metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, color == initialColor);
        editor.presets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [commitColor, color]() {
                             if (commitColor) {
                                 commitColor(color);
                             }
                         });
    }
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateColorEditor(ColorEditor& editor, const QColor& color,
                                                           bool mixed) {
    if (editor.picker != nullptr) {
        const QSignalBlocker blocker(editor.picker);
        editor.picker->setValue(adqt::widgets::AdColorValue::solid(color));
    }
    if (editor.trigger != nullptr) {
        editor.trigger->setSwatchColor(color);
    }
    for (int index = 0; index < editor.presets.size() && index < editor.presetValues.size();
         ++index) {
        setButtonActive(editor.presets.at(index), !mixed && editor.presetValues.at(index) == color);
    }
}

void ScreenshotToolPaletteStyleControls::refreshColorEditorMetrics(
    ColorEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureColorPickerMetrics(editor.picker, metrics);
    if (screenshotToolPaletteMetricsApplyTo(metrics, editor.trigger)) {
        configureScreenshotToolPaletteStyleButton(editor.trigger, nullptr, metrics);
        editor.trigger->setPhysicalScale(metrics.physicalScale);
    }
    for (adqt::widgets::AdButton* button : editor.presets) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        if (auto* swatch = dynamic_cast<ColorSwatchButton*>(button)) {
            swatch->setPhysicalScale(metrics.physicalScale);
        }
    }
}

void ScreenshotToolPaletteStyleControls::addSpotlightColorControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const QColor& initialColor,
    const QVector<QColor>& presetValues, const std::function<void(const QColor&)>& commitColor,
    const std::function<void(const QColor&)>& previewColor,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    ColorEditorConfig config;
    config.accessibleName = QStringLiteral("Mask color");
    config.pickerObjectName = QStringLiteral("screenshotSpotlightColorPicker");
    config.triggerObjectName = QStringLiteral("screenshotSpotlightColorTrigger");
    config.presetValues = presetValues;
    config.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Mask color %1").arg(color.name());
    };
    m_spotlightColorEditor = addColorEditor(layout, parent, receiver, config, initialColor, nullptr,
                                            commitColor, previewColor, metrics);
}

void ScreenshotToolPaletteStyleControls::updateSpotlightColorControls(const QColor& color) {
    updateColorEditor(m_spotlightColorEditor, color, false);
}

ScreenshotToolPaletteStyleControls::FillEditor ScreenshotToolPaletteStyleControls::addFillEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const FillEditorConfig& config,
    const QColor& initialColor, SnowCanvasFillStyle initialStyle, bool* handlingChange,
    const std::function<void(const QColor&)>& setColor,
    const std::function<void(SnowCanvasFillStyle)>& setStyle,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    FillEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return editor;
    }

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    editor.colorValues = config.colorValues;
    editor.styleValues = {
        SnowCanvasFillStyle::Solid,
        SnowCanvasFillStyle::CrossLine,
        SnowCanvasFillStyle::Line,
    };
    editor.picker = createColorPickerShell(parent, config.accessibleName, initialColor, true,
                                           config.observePopup);
    editor.trigger = createScreenshotToolPaletteFillStyleTrigger(
        editor.picker, config.accessibleName.toUtf8().constData(), initialColor, initialStyle,
        metrics);
    configureStylePopupTrigger(editor.trigger, config.accessibleName);
    editor.picker->setTriggerContent(editor.trigger);

    ColorPickerPopupLayout popupContent =
        createColorPickerPopupContent(editor.picker, config.popupObjectName, popupMetrics);
    ColorPickerPopupLayout presetRow =
        addColorPickerPopupRow(popupContent, config.presetRowObjectName, popupMetrics);
    for (const QColor& color : editor.colorValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.colorTooltip ? config.colorTooltip(color)
                                : ScreenshotToolPaletteTranslationText(color.name());
        auto* button = createScreenshotToolPaletteColorButton(presetRow.widget, nullptr, color,
                                                              false, true, popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, color == initialColor);
        editor.colorPresets.push_back(button);
        presetRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setColor, color]() {
            if (setColor) {
                setColor(color);
            }
        });
    }
    presetRow.layout->addStretch(1);
    editor.picker->setPopupContent(popupContent.widget);
    configureColorPickerMetrics(editor.picker, metrics);
    layout->addWidget(editor.picker);

    connectColorPickerChanges(editor.picker, receiver, handlingChange, setColor);

    for (SnowCanvasFillStyle style : editor.styleValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.styleTooltip
                ? config.styleTooltip(style)
                : ScreenshotToolPaletteTranslationText(QString::number(static_cast<int>(style)));
        auto* button = createScreenshotToolPaletteFillStyleButton(parent, nullptr, QColor(), style,
                                                                  false, metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, style == initialStyle);
        editor.styleButtons.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setStyle, style]() {
            if (setStyle) {
                setStyle(style);
            }
        });
    }
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateFillEditor(FillEditor& editor, const QColor& color,
                                                          SnowCanvasFillStyle style,
                                                          bool colorMixed, bool styleMixed) {
    if (editor.picker != nullptr) {
        const QSignalBlocker blocker(editor.picker);
        editor.picker->setValue(adqt::widgets::AdColorValue::solid(color));
    }
    if (editor.trigger != nullptr) {
        editor.trigger->setFillColor(color);
        editor.trigger->setFillStyle(style);
        editor.trigger->setMixed(colorMixed || styleMixed);
    }
    for (int index = 0; index < editor.colorPresets.size() && index < editor.colorValues.size();
         ++index) {
        setButtonActive(editor.colorPresets.at(index),
                        !colorMixed && editor.colorValues.at(index) == color);
    }
    for (int index = 0; index < editor.styleButtons.size() && index < editor.styleValues.size();
         ++index) {
        setButtonActive(editor.styleButtons.at(index),
                        !styleMixed && editor.styleValues.at(index) == style);
    }
}

void ScreenshotToolPaletteStyleControls::refreshFillEditorMetrics(
    FillEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureColorPickerMetrics(editor.picker, metrics);
    configureScreenshotToolPaletteFillStyleTrigger(editor.trigger, metrics);
    for (FillStylePreviewButton* button : editor.styleButtons) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        button->setPhysicalScale(metrics.physicalScale);
    }
}

ScreenshotToolPaletteStyleControls::StrokeEditor
ScreenshotToolPaletteStyleControls::addStrokeEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const StrokeEditorConfig& config,
    const QColor& initialColor, SnowCanvasStrokeStyle initialStyle, bool* handlingChange,
    const std::function<void(const QColor&)>& setColor,
    const std::function<void(SnowCanvasStrokeStyle)>& setStyle,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    StrokeEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return editor;
    }

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    editor.colorValues = config.colorValues;
    editor.styleValues = {
        SnowCanvasStrokeStyle::Solid,
        SnowCanvasStrokeStyle::Dashed,
        SnowCanvasStrokeStyle::Dotted,
    };
    editor.picker =
        createColorPickerShell(parent, config.accessibleName, initialColor, false, false);
    editor.trigger = createScreenshotToolPaletteStrokeStyleTrigger(
        editor.picker, config.accessibleName.toUtf8().constData(), initialColor, initialStyle,
        metrics);
    configureStylePopupTrigger(editor.trigger, config.accessibleName);
    editor.picker->setTriggerContent(editor.trigger);

    ColorPickerPopupLayout popupContent =
        createColorPickerPopupContent(editor.picker, config.popupObjectName, popupMetrics);
    ColorPickerPopupLayout styleRow =
        addColorPickerPopupRow(popupContent, config.styleRowObjectName, popupMetrics);
    for (SnowCanvasStrokeStyle style : editor.styleValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.styleTooltip
                ? config.styleTooltip(style)
                : ScreenshotToolPaletteTranslationText(QString::number(static_cast<int>(style)));
        auto* button = createScreenshotToolPaletteStrokeStyleButton(styleRow.widget, nullptr, style,
                                                                    popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, style == initialStyle);
        editor.styleButtons.push_back(button);
        styleRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setStyle, style]() {
            if (setStyle) {
                setStyle(style);
            }
        });
    }
    styleRow.layout->addStretch(1);
    editor.picker->setPopupContent(popupContent.widget);
    configureColorPickerMetrics(editor.picker, metrics);
    layout->addWidget(editor.picker);

    connectColorPickerChanges(editor.picker, receiver, handlingChange, setColor);

    for (const QColor& color : editor.colorValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.colorTooltip ? config.colorTooltip(color)
                                : ScreenshotToolPaletteTranslationText(color.name());
        auto* button =
            createScreenshotToolPaletteColorButton(parent, nullptr, color, false, true, metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, color == initialColor);
        editor.colorPresets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setColor, color]() {
            if (setColor) {
                setColor(color);
            }
        });
    }
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateStrokeEditor(StrokeEditor& editor,
                                                            const QColor& color,
                                                            SnowCanvasStrokeStyle style,
                                                            bool colorMixed, bool styleMixed) {
    if (editor.picker != nullptr) {
        const QSignalBlocker blocker(editor.picker);
        editor.picker->setValue(adqt::widgets::AdColorValue::solid(color));
    }
    if (editor.trigger != nullptr) {
        editor.trigger->setStrokeColor(color);
        editor.trigger->setStrokeStyle(style);
        editor.trigger->setMixed(colorMixed || styleMixed);
    }
    for (int index = 0; index < editor.colorPresets.size() && index < editor.colorValues.size();
         ++index) {
        setButtonActive(editor.colorPresets.at(index),
                        !colorMixed && editor.colorValues.at(index) == color);
    }
    for (int index = 0; index < editor.styleButtons.size() && index < editor.styleValues.size();
         ++index) {
        setButtonActive(editor.styleButtons.at(index),
                        !styleMixed && editor.styleValues.at(index) == style);
    }
}

void ScreenshotToolPaletteStyleControls::refreshStrokeEditorMetrics(
    StrokeEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureColorPickerMetrics(editor.picker, metrics);
    configureScreenshotToolPaletteStrokeStyleTrigger(editor.trigger, metrics);
    for (adqt::widgets::AdButton* button : editor.colorPresets) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        if (auto* swatch = dynamic_cast<ColorSwatchButton*>(button)) {
            swatch->setPhysicalScale(metrics.physicalScale);
        }
    }
}

ScreenshotToolPaletteStyleControls::NumericPresetEditor
ScreenshotToolPaletteStyleControls::addNumericPresetEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const NumericPresetEditorConfig& config,
    double initialValue, const std::function<void()>& cycleValue,
    const std::function<void(double)>& setValue,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    NumericPresetEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return editor;
    }

    editor.values = config.values;
    editor.strokePreview = config.strokePreview;
    editor.summary =
        config.strokePreview
            ? static_cast<adqt::widgets::AdButton*>(createScreenshotToolPaletteStrokeWidthButton(
                  parent, config.summaryTooltip.toUtf8().constData(), initialValue, true, metrics))
            : static_cast<adqt::widgets::AdButton*>(createScreenshotToolPaletteNumericValueButton(
                  parent, config.summaryTooltip.toUtf8().constData(), initialValue, config.suffix,
                  metrics));
    if (!config.summaryObjectName.isEmpty()) {
        editor.summary->setObjectName(config.summaryObjectName);
    }
    layout->addWidget(editor.summary);
    if (cycleValue) {
        QObject::connect(editor.summary, &adqt::widgets::AdButton::clicked, receiver, cycleValue);
    }

    for (int index = 0; index < editor.values.size(); ++index) {
        const double value = editor.values.at(index);
        const ScreenshotToolPaletteTranslationText tooltip =
            config.presetTooltip ? config.presetTooltip(index, value)
                                 : ScreenshotToolPaletteTranslationText(QString::number(value));
        adqt::widgets::AdButton* button = nullptr;
        if (config.strokePreview) {
            button = createScreenshotToolPaletteStrokeWidthButton(parent, nullptr, value, false,
                                                                  metrics);
        } else {
            button = createScreenshotToolPaletteStyleActionButton(
                parent, nullptr,
                config.presetIcon ? config.presetIcon(index) : adqt::icons::IconRef(), metrics);
        }
        configureScreenshotToolPaletteTooltip(button, tooltip);
        if (config.presetObjectName) {
            button->setObjectName(config.presetObjectName(value));
        }
        editor.presets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setValue, value]() {
            if (setValue) {
                setValue(value);
            }
        });
    }
    updateNumericPresetEditor(editor, initialValue, false);
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateNumericPresetEditor(NumericPresetEditor& editor,
                                                                   double value, bool mixed) {
    if (auto* strokeSummary = dynamic_cast<StrokeWidthPreviewButton*>(editor.summary)) {
        strokeSummary->setStrokeWidth(value);
        strokeSummary->setActiveStrokeWidth(true);
        strokeSummary->setMixed(mixed);
    } else if (auto* numericSummary = dynamic_cast<NumericValuePreviewButton*>(editor.summary)) {
        numericSummary->setValue(value);
        numericSummary->setMixed(mixed);
    }
    for (int index = 0; index < editor.presets.size() && index < editor.values.size(); ++index) {
        const bool active = !mixed && qFuzzyCompare(editor.values.at(index) + 1.0, value + 1.0);
        setButtonActive(editor.presets.at(index), active);
        if (auto* strokeButton =
                dynamic_cast<StrokeWidthPreviewButton*>(editor.presets.at(index))) {
            strokeButton->setStrokeWidth(editor.values.at(index));
            strokeButton->setActiveStrokeWidth(active);
        }
    }
}

void ScreenshotToolPaletteStyleControls::refreshNumericPresetEditorMetrics(
    NumericPresetEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (screenshotToolPaletteMetricsApplyTo(metrics, editor.summary)) {
        configureScreenshotToolPaletteStyleButton(editor.summary, nullptr, metrics);
        if (auto* stroke = dynamic_cast<StrokeWidthPreviewButton*>(editor.summary)) {
            stroke->setPhysicalScale(metrics.physicalScale);
        }
        if (auto* numeric = dynamic_cast<NumericValuePreviewButton*>(editor.summary)) {
            numeric->setPhysicalScale(metrics.physicalScale);
        }
    }
    for (adqt::widgets::AdButton* button : editor.presets) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        if (auto* stroke = dynamic_cast<StrokeWidthPreviewButton*>(button)) {
            stroke->setPhysicalScale(metrics.physicalScale);
        }
    }
}

void ScreenshotToolPaletteStyleControls::addPenFilterStrokeWidthControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, double initialWidth,
    const std::function<void()>& cycleWidth, const std::function<void(double)>& setWidth,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    const QStringList labels{
        QStringLiteral("S"),
        QStringLiteral("M"),
        QStringLiteral("L"),
        QStringLiteral("XL"),
    };
    NumericPresetEditorConfig config;
    config.summaryTooltip = QStringLiteral("Current pen filter stroke width");
    config.summaryObjectName = QStringLiteral("screenshotPenFilterStrokeWidthSummary");
    config.suffix = QStringLiteral("px");
    config.values = {24.0, 30.0, 42.0, 54.0};
    config.presetTooltip = [labels](int index, double value) {
        return ScreenshotToolPaletteTranslationText("Pen filter stroke width %1 (%2px)")
            .arg(labels.value(index))
            .arg(value, 0, 'g', 3);
    };
    config.presetObjectName = [](double value) {
        return QStringLiteral("screenshotPenFilterStrokeWidth%1").arg(qRound(value));
    };
    config.presetIcon = [](int index) { return fontSizePresetIcon(index); };
    m_penFilterStrokeWidthEditor = addNumericPresetEditor(
        layout, parent, receiver, config, initialWidth, cycleWidth, setWidth, metrics);
}

void ScreenshotToolPaletteStyleControls::updatePenFilterStrokeWidthControls(double width,
                                                                            bool mixed) {
    updateNumericPresetEditor(m_penFilterStrokeWidthEditor, width, mixed);
}

ScreenshotToolPaletteStyleControls::WidthColorEditor
ScreenshotToolPaletteStyleControls::addWidthColorEditor(
    QBoxLayout* layout, QWidget* parent, QObject* receiver, const WidthColorEditorConfig& config,
    double initialWidth, const QColor& initialColor, bool* handlingChange,
    const std::function<void(double)>& setWidth, const std::function<void(const QColor&)>& setColor,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    WidthColorEditor editor;
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return editor;
    }

    editor.widthValues = config.widthValues;
    editor.colorValues = config.colorValues;
    editor.picker = createColorPickerShell(parent, config.accessibleName, initialColor, false,
                                           config.observePopup);

    editor.trigger = createScreenshotToolPaletteStrokeWidthButton(
        editor.picker, config.triggerTooltip.toUtf8().constData(), initialWidth, true, metrics);
    configureStylePopupTrigger(editor.trigger, config.accessibleName);
    editor.picker->setTriggerContent(editor.trigger);

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    ColorPickerPopupLayout popupContent =
        createColorPickerPopupContent(editor.picker, config.popupObjectName, popupMetrics);
    ColorPickerPopupLayout widthRow =
        addColorPickerPopupRow(popupContent, config.widthRowObjectName, popupMetrics);
    for (double width : editor.widthValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.widthTooltip ? config.widthTooltip(width)
                                : ScreenshotToolPaletteTranslationText(QString::number(width));
        auto* button = createScreenshotToolPaletteStrokeWidthButton(widthRow.widget, nullptr, width,
                                                                    false, popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        editor.widthButtons.push_back(button);
        widthRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setWidth, width]() {
            if (setWidth) {
                setWidth(width);
            }
        });
    }
    widthRow.layout->addStretch(1);

    ColorPickerPopupLayout colorRow =
        addColorPickerPopupRow(popupContent, config.colorRowObjectName, popupMetrics);
    for (const QColor& color : editor.colorValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.colorTooltip ? config.colorTooltip(color)
                                : ScreenshotToolPaletteTranslationText(color.name());
        auto* button = createScreenshotToolPaletteColorButton(colorRow.widget, nullptr, color,
                                                              false, true, popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        editor.colorButtons.push_back(button);
        colorRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver, [setColor, color]() {
            if (setColor) {
                setColor(color);
            }
        });
    }
    colorRow.layout->addStretch(1);
    editor.picker->setPopupContent(popupContent.widget);
    configureColorPickerMetrics(editor.picker, metrics);
    layout->addWidget(editor.picker);

    connectColorPickerChanges(editor.picker, receiver, handlingChange, setColor);
    updateWidthColorEditor(editor, initialWidth, initialColor, false, false);
    return editor;
}

void ScreenshotToolPaletteStyleControls::updateWidthColorEditor(WidthColorEditor& editor,
                                                                double width, const QColor& color,
                                                                bool widthMixed, bool colorMixed) {
    if (editor.picker != nullptr) {
        const QSignalBlocker blocker(editor.picker);
        editor.picker->setValue(adqt::widgets::AdColorValue::solid(color));
        editor.picker->setProperty("mixed", colorMixed);
    }
    if (editor.trigger != nullptr) {
        editor.trigger->setStrokeWidth(width);
        editor.trigger->setMixed(widthMixed || colorMixed);
    }
    for (int index = 0; index < editor.widthButtons.size() && index < editor.widthValues.size();
         ++index) {
        setButtonActive(editor.widthButtons.at(index),
                        !widthMixed &&
                            qFuzzyCompare(editor.widthValues.at(index) + 1.0, width + 1.0));
    }
    for (int index = 0; index < editor.colorButtons.size() && index < editor.colorValues.size();
         ++index) {
        setButtonActive(editor.colorButtons.at(index),
                        !colorMixed && editor.colorValues.at(index) == color);
    }
}

void ScreenshotToolPaletteStyleControls::refreshWidthColorEditorMetrics(
    WidthColorEditor& editor, const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureColorPickerMetrics(editor.picker, metrics);
    if (screenshotToolPaletteMetricsApplyTo(metrics, editor.trigger)) {
        configureScreenshotToolPaletteStyleButton(editor.trigger, nullptr, metrics);
        editor.trigger->setPhysicalScale(metrics.physicalScale);
    }
}

void ScreenshotToolPaletteStyleControls::addShapeControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    ScreenshotToolPaletteRadioEditorConfig config;
    config.objectName = QStringLiteral("screenshotShapeButtonGroup");
    config.options = {
        {0, QStringLiteral("Rectangle"), custom_outlined_icons::ShapeRectangle()},
        {1, QStringLiteral("Ellipse"), custom_outlined_icons::ShapeEllipse()},
        {2, QStringLiteral("Diamond"), custom_outlined_icons::ShapeDiamond()},
    };
    config.initialId = 0;
    const ScreenshotToolPaletteRadioEditor editor =
        createScreenshotToolPaletteRadioEditor(parent, config, metrics);
    m_shapeControlsContainer = editor.container;
    m_shapeButtonGroup = editor.group;
    m_rectangleShapeButton = editor.buttons.value(0);
    m_ellipseShapeButton = editor.buttons.value(1);
    m_diamondShapeButton = editor.buttons.value(2);
    QObject::connect(m_shapeButtonGroup, &adqt::widgets::AdRadioButtonGroup::checkedIdChanged,
                     receiver, [this](int id) {
                         setShape(id == 1   ? SnowCanvasRectangleShape::Ellipse
                                  : id == 2 ? SnowCanvasRectangleShape::Diamond
                                            : SnowCanvasRectangleShape::Rectangle);
                     });
    layout->addWidget(m_shapeControlsContainer);
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

void ScreenshotToolPaletteStyleControls::addStrokeWidthControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    NumericPresetEditorConfig strokeWidthConfig;
    strokeWidthConfig.summaryTooltip = QStringLiteral("Current stroke width");
    strokeWidthConfig.values = m_rectangleStyle.strokeWidthValues();
    strokeWidthConfig.strokePreview = true;
    strokeWidthConfig.presetTooltip = [](int, double value) {
        return ScreenshotToolPaletteTranslationText("Stroke width %1").arg(value, 0, 'g', 2);
    };
    m_shapeStrokeWidthEditor = addNumericPresetEditor(
        layout, parent, receiver, strokeWidthConfig, m_rectangleStyle.strokeWidth(),
        [this]() { cycleStrokeWidth(); }, [this](double value) { setStrokeWidth(value); }, metrics);
}

void ScreenshotToolPaletteStyleControls::addStrokeColorControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    StrokeEditorConfig strokeConfig;
    strokeConfig.accessibleName = QStringLiteral("Stroke color");
    strokeConfig.popupObjectName = QStringLiteral("screenshotStrokeOptions");
    strokeConfig.styleRowObjectName = QStringLiteral("screenshotStrokeStyles");
    strokeConfig.colorValues = m_rectangleStyle.strokeColorValues();
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
    m_shapeStrokeEditor = addStrokeEditor(
        layout, parent, receiver, strokeConfig, m_rectangleStyle.strokeColor(),
        m_rectangleStyle.strokeStyle(), &m_handlingStrokeColorPickerChange,
        [this](const QColor& color) { setStrokeColor(color); },
        [this](SnowCanvasStrokeStyle style) { setStrokeStyle(style); }, metrics);
}

void ScreenshotToolPaletteStyleControls::addFillColorControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    FillEditorConfig fillConfig;
    fillConfig.accessibleName = QStringLiteral("Fill color");
    fillConfig.popupObjectName = QStringLiteral("screenshotFillOptions");
    fillConfig.presetRowObjectName = QStringLiteral("screenshotFillColorPresets");
    fillConfig.colorValues = m_rectangleStyle.fillColorValues();
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
    m_shapeFillEditor = addFillEditor(
        layout, parent, receiver, fillConfig, m_rectangleStyle.fillColor(),
        m_rectangleStyle.fillStyle(), nullptr, [this](const QColor& color) { setFillColor(color); },
        [this](SnowCanvasFillStyle style) { setFillStyle(style); }, metrics);
}

void ScreenshotToolPaletteStyleControls::addCornerRadiusControl(
    QBoxLayout* layout, QWidget* parent, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr) {
        return;
    }

    m_cornerRadiusEditor = createScreenshotToolPaletteCornerRadiusEditor(
        parent, "Corner radius (scroll to adjust)", custom_outlined_icons::SelectionRadius(),
        m_rectangleStyle.cornerRadius(), metrics);
    m_cornerRadiusEditor->setObjectName(
        QStringLiteral("screenshotSelectionCornerRadiusButton"));
    QObject::connect(m_cornerRadiusEditor, &adqt::widgets::AdButton::clicked, parent,
                     [this]() { setCornerRadius(defaultCornerRadius()); });
    layout->addWidget(m_cornerRadiusEditor);
}

void ScreenshotToolPaletteStyleControls::addHighlightControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const std::function<void()>& addGroupSeparator,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    ColorEditorConfig highlightColorConfig;
    highlightColorConfig.accessibleName = QStringLiteral("Highlight color");
    highlightColorConfig.presetValues = m_textStyle.colorValues();
    highlightColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Highlight color %1").arg(color.name());
    };
    m_highlightColorEditor = addColorEditor(
        layout, parent, receiver, highlightColorConfig, m_highlightStyle.fillColor(),
        &m_handlingHighlightColorPickerChange, [this](const QColor& color) { setFillColor(color); },
        {}, metrics);
    if (addGroupSeparator) {
        addGroupSeparator();
    }

    WidthColorEditorConfig highlightStrokeConfig;
    highlightStrokeConfig.accessibleName = QStringLiteral("Highlight stroke width");
    highlightStrokeConfig.triggerTooltip =
        QStringLiteral("Highlight stroke width");
    highlightStrokeConfig.popupObjectName = QStringLiteral("screenshotHighlightStrokeOptions");
    highlightStrokeConfig.widthRowObjectName =
        QStringLiteral("screenshotHighlightStrokeWidthPresets");
    highlightStrokeConfig.colorRowObjectName =
        QStringLiteral("screenshotHighlightStrokeColorPresets");
    highlightStrokeConfig.widthValues = m_highlightStyle.strokeWidthValues();
    highlightStrokeConfig.colorValues = m_highlightStyle.strokeColorValues();
    highlightStrokeConfig.widthTooltip = [](double width) {
        return ScreenshotToolPaletteTranslationText("Highlight stroke width %1px")
            .arg(width, 0, 'g', 3);
    };
    highlightStrokeConfig.colorTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Highlight stroke color %1").arg(color.name());
    };
    m_highlightStrokeEditor = addWidthColorEditor(
        layout, parent, receiver, highlightStrokeConfig, m_highlightStyle.strokeWidth(),
        m_highlightStyle.strokeColor(), &m_handlingHighlightStrokeColorPickerChange,
        [this](double width) { setStrokeWidth(width); },
        [this](const QColor& color) { setStrokeColor(color); }, metrics);

    setHighlightControlsActive(false);
}

void ScreenshotToolPaletteStyleControls::addPenHighlightControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const std::function<void()>& addGroupSeparator,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    ColorEditorConfig penHighlightColorConfig;
    penHighlightColorConfig.accessibleName = QStringLiteral("Pen highlight color");
    penHighlightColorConfig.presetValues = m_textStyle.colorValues();
    penHighlightColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Pen highlight color %1").arg(color.name());
    };
    m_penHighlightColorEditor = addColorEditor(
        layout, parent, receiver, penHighlightColorConfig, m_penHighlightStyle.stroke,
        &m_handlingPenHighlightColorPickerChange,
        [this](const QColor& color) { setPenHighlightColor(color); }, {}, metrics);

    if (addGroupSeparator) {
        addGroupSeparator();
    }
    const QVector<double> widths{24.0, 30.0, 42.0, 54.0};
    const QStringList labels{
        QStringLiteral("S"),
        QStringLiteral("M"),
        QStringLiteral("L"),
        QStringLiteral("XL"),
    };
    NumericPresetEditorConfig penHighlightWidthConfig;
    penHighlightWidthConfig.summaryTooltip = QStringLiteral("Current pen highlight stroke width");
    penHighlightWidthConfig.suffix = QStringLiteral("px");
    penHighlightWidthConfig.values = widths;
    penHighlightWidthConfig.presetTooltip = [labels](int index, double value) {
        return ScreenshotToolPaletteTranslationText("Pen highlight stroke width %1 (%2px)")
            .arg(labels.value(index))
            .arg(value, 0, 'g', 3);
    };
    penHighlightWidthConfig.presetIcon = [](int index) { return fontSizePresetIcon(index); };
    m_penHighlightStrokeWidthEditor = addNumericPresetEditor(
        layout, parent, receiver, penHighlightWidthConfig, m_penHighlightStyle.strokeWidth, {},
        [this](double value) { setPenHighlightStrokeWidth(value); }, metrics);
    updatePenHighlightStyleControls();
}

void ScreenshotToolPaletteStyleControls::addArrowControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const std::function<void()>& addGroupSeparator,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    StrokeEditorConfig arrowStrokeConfig;
    arrowStrokeConfig.accessibleName = QStringLiteral("Arrow stroke color");
    arrowStrokeConfig.colorValues = arrowStrokeColorValues();
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
    m_arrowStrokeEditor = addStrokeEditor(
        layout, parent, receiver, arrowStrokeConfig, m_arrowStyle.stroke, m_arrowStyle.strokeStyle,
        &m_handlingArrowStrokeColorPickerChange,
        [this](const QColor& color) { setArrowStrokeColor(color); },
        [this](SnowCanvasStrokeStyle style) { setArrowStrokeStyle(style); }, metrics);

    if (addGroupSeparator) {
        addGroupSeparator();
    }

    NumericPresetEditorConfig arrowStrokeWidthConfig;
    arrowStrokeWidthConfig.summaryTooltip = QStringLiteral("Current arrow stroke width");
    arrowStrokeWidthConfig.values = arrowStrokeWidthValues();
    arrowStrokeWidthConfig.strokePreview = true;
    arrowStrokeWidthConfig.presetTooltip = [](int, double value) {
        return ScreenshotToolPaletteTranslationText("Arrow stroke width %1").arg(value, 0, 'g', 2);
    };
    m_arrowStrokeWidthEditor = addNumericPresetEditor(
        layout, parent, receiver, arrowStrokeWidthConfig, m_arrowStyle.strokeWidth,
        [this]() { cycleArrowStrokeWidth(); }, [this](double value) { setArrowStrokeWidth(value); },
        metrics);

    if (addGroupSeparator) {
        addGroupSeparator();
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
        createScreenshotToolPaletteRadioEditor(parent, arrowTypeConfig, metrics);
    m_arrowTypeControlsContainer = arrowTypeEditor.container;
    m_arrowTypeButtonGroup = arrowTypeEditor.group;
    QObject::connect(m_arrowTypeButtonGroup, &adqt::widgets::AdRadioButtonGroup::checkedIdChanged,
                     receiver, [this](int id) {
                         setArrowType(id == 1   ? SnowCanvasArrowType::Curve
                                      : id == 2 ? SnowCanvasArrowType::Elbow
                                                : SnowCanvasArrowType::Straight);
                     });
    layout->addWidget(m_arrowTypeControlsContainer);

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
    const auto addArrowheadEditor = [this, layout, parent, receiver, metrics,
                                     &arrowheads](bool start, const QString& accessibleName) {
        IconOptionEditorConfig config;
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
            start ? m_arrowStyle.startArrowhead : m_arrowStyle.endArrowhead;
        return addIconOptionEditor(
            layout, parent, receiver, config, static_cast<int>(currentArrowhead),
            [this, start](int value) {
                setArrowhead(start, static_cast<SnowCanvasArrowhead>(value));
            },
            metrics);
    };
    m_startArrowheadEditor = addArrowheadEditor(true, QStringLiteral("Start arrowhead"));
    m_endArrowheadEditor = addArrowheadEditor(false, QStringLiteral("End arrowhead"));

    updateArrowStyleControls();
}

void ScreenshotToolPaletteStyleControls::addTextControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const std::function<void()>& addGroupSeparator,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    const SnowCanvasTextStyle& style = m_textStyle.textStyle();

    ColorEditorConfig textColorConfig;
    textColorConfig.accessibleName = QStringLiteral("Text color");
    textColorConfig.presetValues = m_textStyle.colorValues();
    textColorConfig.observePopup = true;
    textColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Text color %1").arg(color.name());
    };
    m_textColorEditor = addColorEditor(
        layout, parent, receiver, textColorConfig, style.color, &m_handlingTextColorPickerChange,
        [this](const QColor& color) { setTextColor(color); }, {}, metrics);

    addGroupSeparator();
    const QStringList fontSizeLabels{
        QStringLiteral("S"),
        QStringLiteral("M"),
        QStringLiteral("L"),
        QStringLiteral("XL"),
    };
    FontEditorConfig textFontConfig;
    textFontConfig.accessibleName = QStringLiteral("Text font family");
    textFontConfig.summaryTooltip = QStringLiteral("Current text font size");
    textFontConfig.sizeValues = m_textStyle.fontSizeValues();
    textFontConfig.observePopup = true;
    textFontConfig.presetTooltip = [fontSizeLabels](int index, double value) {
        return ScreenshotToolPaletteTranslationText("Text font size %1 (%2px)")
            .arg(fontSizeLabels.value(index))
            .arg(value, 0, 'g', 3);
    };
    m_textFontEditor = addFontEditor(
        layout, parent, receiver, textFontConfig, style.fontSize, style.fontFamily,
        [this]() { cycleTextFontSize(); }, [this](double value) { setTextFontSize(value); },
        [this](const QString& value) { setTextFontFamily(value); }, metrics);

    IconOptionEditorConfig alignmentConfig;
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
    m_textAlignmentEditor = addIconOptionEditor(
        layout, parent, receiver, alignmentConfig, static_cast<int>(style.horizontalAlign),
        [this](int value) {
            setTextHorizontalAlign(static_cast<SnowCanvasTextHorizontalAlign>(value));
        },
        metrics);

    addGroupSeparator();
    WidthColorEditorConfig textStrokeConfig;
    textStrokeConfig.accessibleName = QStringLiteral("Text stroke width");
    textStrokeConfig.triggerTooltip = QStringLiteral("Text stroke width");
    textStrokeConfig.popupObjectName = QStringLiteral("screenshotTextStrokeWidthOptions");
    textStrokeConfig.widthRowObjectName = QStringLiteral("screenshotTextStrokeWidthPresets");
    textStrokeConfig.colorRowObjectName = QStringLiteral("screenshotTextStrokeColorPresets");
    textStrokeConfig.widthValues = m_textStyle.strokeWidthValues();
    textStrokeConfig.colorValues = m_textStyle.fillColorValues();
    textStrokeConfig.observePopup = true;
    textStrokeConfig.widthTooltip = [](double width) {
        return ScreenshotToolPaletteTranslationText("Text stroke width %1px").arg(width, 0, 'g', 3);
    };
    textStrokeConfig.colorTooltip = [](const QColor& color) {
        return color.alpha() == 0
                   ? ScreenshotToolPaletteTranslationText("Text stroke color transparent")
                   : ScreenshotToolPaletteTranslationText("Text stroke color %1").arg(color.name());
    };
    m_textStrokeEditor = addWidthColorEditor(
        layout, parent, receiver, textStrokeConfig, style.strokeWidth, style.stroke,
        &m_handlingTextStrokeColorPickerChange, [this](double width) { setTextStrokeWidth(width); },
        [this](const QColor& color) { setTextStrokeColor(color); }, metrics);
    addToolbarSpacing(layout, kTextStrokeColorTrailingSpacing, metrics);

    FillEditorConfig textFillConfig;
    textFillConfig.accessibleName = QStringLiteral("Text fill color");
    textFillConfig.popupObjectName = QStringLiteral("screenshotTextFillOptions");
    textFillConfig.presetRowObjectName = QStringLiteral("screenshotTextFillColorPresets");
    textFillConfig.colorValues = m_textStyle.fillColorValues();
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
    m_textFillEditor = addFillEditor(
        layout, parent, receiver, textFillConfig, style.fill, style.fillStyle,
        &m_handlingTextFillColorPickerChange,
        [this](const QColor& color) { setTextFillColor(color); },
        [this](SnowCanvasFillStyle fillStyle) { setTextFillStyle(fillStyle); }, metrics);
    addToolbarSpacing(layout, kCornerRadiusLeadingSpacing, metrics);
    m_textCornerRadiusEditor = createScreenshotToolPaletteCornerRadiusEditor(
        parent, "Text fill corner radius (scroll to adjust)",
        custom_outlined_icons::SelectionRadius(), qRound(style.cornerRadii.topLeft), metrics);
    QObject::connect(m_textCornerRadiusEditor, &adqt::widgets::AdButton::clicked, receiver,
                     [this]() { setTextCornerRadius(defaultCornerRadius()); });
    layout->addWidget(m_textCornerRadiusEditor);

    updateTextStyleControls();
}

void ScreenshotToolPaletteStyleControls::addWatermarkControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const std::function<void()>& addGroupSeparator,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    const SnowCanvasWatermarkConfig& config = m_watermarkConfig;
    ColorEditorConfig watermarkColorConfig;
    watermarkColorConfig.accessibleName = QStringLiteral("Watermark color");
    watermarkColorConfig.pickerObjectName = QStringLiteral("screenshotWatermarkColorPicker");
    watermarkColorConfig.triggerObjectName = QStringLiteral("screenshotWatermarkColorTrigger");
    watermarkColorConfig.presetValues = m_textStyle.colorValues();
    watermarkColorConfig.observePopup = true;
    watermarkColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Watermark color %1").arg(color.name());
    };
    m_watermarkColorEditor = addColorEditor(
        layout, parent, receiver, watermarkColorConfig, config.color, nullptr,
        [this](const QColor& color) { setWatermarkColor(color); },
        [this](const QColor& color) {
            if (m_watermarkConfig.color == color) {
                return;
            }
            m_watermarkConfig.color = color;
            m_watermarkColorPreviewPending = true;
            notifyWatermarkPreviewChanged();
        },
        metrics);

    addGroupSeparator();

    m_watermarkTextEdit = new adqt::widgets::AdLineEdit(parent);
    m_watermarkTextEdit->setFocusPolicy(Qt::ClickFocus);
    m_watermarkTextEdit->setObjectName(QStringLiteral("screenshotWatermarkTextEdit"));
    setScreenshotToolPalettePlaceholderSource(m_watermarkTextEdit, "Watermark Text");
    setScreenshotToolPaletteTooltipSource(m_watermarkTextEdit, "Watermark Text");
    setScreenshotToolPaletteAccessibleNameSource(m_watermarkTextEdit, "Watermark Text");
    const QString watermarkTextLabel =
        QCoreApplication::translate("ScreenshotToolPalette", "Watermark Text");
    m_watermarkTextEdit->setPlaceholderText(watermarkTextLabel);
    m_watermarkTextEdit->setToolTip(watermarkTextLabel);
    m_watermarkTextEdit->setAccessibleName(watermarkTextLabel);
    m_watermarkTextEdit->setControlSize(adqt::widgets::AdLineEdit::ControlSize::Small);
    m_watermarkTextEdit->setVariant(adqt::widgets::AdLineEdit::Variant::Borderless);
    m_watermarkTextEdit->setFixedSize(qMax(1, qRound(135.0 * metrics.physicalScale)),
                                      qMax(1, qRound(metrics.buttonSize * metrics.physicalScale)));
    m_watermarkTextEdit->setText(config.text);
    layout->addWidget(m_watermarkTextEdit);
    QObject::connect(m_watermarkTextEdit, &QLineEdit::textChanged, receiver,
                     [this](const QString& text) {
                         const QString normalized = text.trimmed();
                         if (m_watermarkConfig.text == normalized) {
                             return;
                         }
                         m_watermarkConfig.text = normalized;
                         notifyWatermarkConfigChanged();
                     });
    QObject::connect(m_watermarkTextEdit, &QLineEdit::editingFinished, receiver, [this]() {
        const QString normalized = m_watermarkTextEdit->text().trimmed();
        m_watermarkConfig.text = normalized;
        const QSignalBlocker blocker(m_watermarkTextEdit);
        m_watermarkTextEdit->setText(normalized);
    });

    const QStringList fontSizeLabels{
        QStringLiteral("S"),
        QStringLiteral("M"),
        QStringLiteral("L"),
        QStringLiteral("XL"),
    };
    FontEditorConfig watermarkFontConfig;
    watermarkFontConfig.accessibleName = QStringLiteral("Watermark font family");
    watermarkFontConfig.summaryTooltip = QStringLiteral("Current watermark font size");
    watermarkFontConfig.summaryObjectName =
        QStringLiteral("screenshotWatermarkFontSizeSummaryButton");
    watermarkFontConfig.sizeValues = watermarkFontSizeValues();
    watermarkFontConfig.observePopup = true;
    watermarkFontConfig.presetTooltip = [fontSizeLabels](int index, double value) {
        return ScreenshotToolPaletteTranslationText("Watermark font size %1 (%2px)")
            .arg(fontSizeLabels.value(index))
            .arg(value, 0, 'g', 3);
    };
    m_watermarkFontEditor = addFontEditor(
        layout, parent, receiver, watermarkFontConfig, config.fontSize, config.fontFamily,
        [this]() { cycleWatermarkFontSize(); },
        [this](double value) { setWatermarkFontSize(value); },
        [this](const QString& value) { setWatermarkFontFamily(value); }, metrics);

    addGroupSeparator();

    m_watermarkAngleEditor = createScreenshotToolPaletteIconNumericValueButton(
        parent, "Watermark angle", custom_outlined_icons::Angle(),
        qBound(-90, qRound(config.angle), 90), QStringLiteral("-90"), metrics);
    m_watermarkAngleEditor->setObjectName(QStringLiteral("screenshotWatermarkAngleEditor"));
    layout->addWidget(m_watermarkAngleEditor);
    QObject::connect(m_watermarkAngleEditor, &adqt::widgets::AdButton::clicked, receiver,
                     [this]() { setWatermarkAngle(30.0); });

    m_watermarkGapEditor = createScreenshotToolPaletteIconNumericValueButton(
        parent, "Watermark gap", custom_outlined_icons::WatermarkGap(),
        qBound(10, qRound(config.gap), 200), QStringLiteral("200"), metrics);
    m_watermarkGapEditor->setObjectName(QStringLiteral("screenshotWatermarkGapEditor"));
    layout->addWidget(m_watermarkGapEditor);
    QObject::connect(m_watermarkGapEditor, &adqt::widgets::AdButton::clicked, receiver,
                     [this]() { setWatermarkGap(56.0); });

    addGroupSeparator();

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
        createScreenshotToolPaletteSliderEditor(layout, parent, opacityConfig, metrics);
    QObject::connect(m_watermarkOpacityEditor.slider, &adqt::widgets::AdSlider::valueChanged,
                     receiver, [this](double value) { setWatermarkOpacity(value / 100.0); });
    refreshWatermarkOpacityMetrics(metrics);

    updateWatermarkControls();
}

void ScreenshotToolPaletteStyleControls::addSerialNumberControls(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const std::function<void()>& addGroupSeparator,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    ColorEditorConfig serialNumberColorConfig;
    serialNumberColorConfig.accessibleName = QStringLiteral("Sequence number color");
    serialNumberColorConfig.presetValues = m_textStyle.colorValues();
    serialNumberColorConfig.presetTooltip = [](const QColor& color) {
        return ScreenshotToolPaletteTranslationText("Sequence number color %1").arg(color.name());
    };
    m_serialNumberColorEditor = addColorEditor(
        layout, parent, receiver, serialNumberColorConfig, m_serialNumberStyle.color,
        &m_handlingSerialNumberColorPickerChange,
        [this](const QColor& color) { setSerialNumberColor(color); }, {}, metrics);

    addGroupSeparator();
    m_serialNumberEditor = createScreenshotToolPaletteCornerRadiusEditor(
        parent, "Sequence number (scroll to adjust)", outlined_icons::Number(),
        static_cast<int>(
            std::clamp<qint64>(m_serialNumberStyle.number, 0, std::numeric_limits<int>::max())),
        metrics);
    layout->addWidget(m_serialNumberEditor);
    addToolbarSpacing(layout, kSerialNumberTrailingSpacing, metrics);

    FontEditorConfig serialNumberFontConfig;
    serialNumberFontConfig.accessibleName = QStringLiteral("Sequence number font family");
    serialNumberFontConfig.summaryTooltip = QStringLiteral("Current sequence number font size");
    serialNumberFontConfig.sizeValues = m_textStyle.fontSizeValues();
    serialNumberFontConfig.presetTooltip = [](int, double value) {
        return ScreenshotToolPaletteTranslationText("Sequence number font size %1px")
            .arg(value, 0, 'g', 3);
    };
    m_serialNumberFontEditor = addFontEditor(
        layout, parent, receiver, serialNumberFontConfig, m_serialNumberStyle.fontSize,
        m_serialNumberStyle.fontFamily, [this]() { cycleSerialNumberFontSize(); },
        [this](double value) { setSerialNumberFontSize(value); },
        [this](const QString& value) { setSerialNumberFontFamily(value); }, metrics);

    addGroupSeparator();
    FillEditorConfig serialNumberFillConfig;
    serialNumberFillConfig.accessibleName = QStringLiteral("Sequence number fill color");
    serialNumberFillConfig.popupObjectName = QStringLiteral("screenshotSerialNumberFillOptions");
    serialNumberFillConfig.presetRowObjectName =
        QStringLiteral("screenshotSerialNumberFillColorPresets");
    serialNumberFillConfig.colorValues = m_textStyle.fillColorValues();
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
    m_serialNumberFillEditor = addFillEditor(
        layout, parent, receiver, serialNumberFillConfig, m_serialNumberStyle.fill,
        m_serialNumberStyle.fillStyle, &m_handlingSerialNumberFillColorPickerChange,
        [this](const QColor& color) { setSerialNumberFillColor(color); },
        [this](SnowCanvasFillStyle fillStyle) { setSerialNumberFillStyle(fillStyle); }, metrics);
    updateSerialNumberStyleControls();
}

void ScreenshotToolPaletteStyleControls::reset() {
    clearTextStylePopupInteractions();
    ScreenshotToolPaletteStyleState::reset(m_defaults);
    updateRectangleStyleControls();
    updateArrowStyleControls();
    updateTextStyleControls();
    updateWatermarkControls();
    updateSerialNumberStyleControls();
}

void ScreenshotToolPaletteStyleControls::releaseControlBindings() {
    clearTextStylePopupInteractions();

    m_handlingStrokeColorPickerChange = false;
    m_handlingArrowStrokeColorPickerChange = false;
    m_handlingTextColorPickerChange = false;
    m_handlingTextStrokeColorPickerChange = false;
    m_handlingTextFillColorPickerChange = false;
    m_handlingHighlightColorPickerChange = false;
    m_handlingHighlightStrokeColorPickerChange = false;
    m_handlingPenHighlightColorPickerChange = false;
    m_handlingSerialNumberColorPickerChange = false;
    m_handlingSerialNumberFillColorPickerChange = false;
    m_arrowControlsActive = false;
    m_lineControlsActive = false;
    m_freeDrawControlsActive = false;
    m_highlightControlsActive = false;
    m_penHighlightControlsActive = false;
    m_textControlsActive = false;

    m_shapeStrokeWidthEditor = {};
    m_shapeStrokeEditor = {};
    m_shapeFillEditor = {};
    m_cornerRadiusEditor = nullptr;
    m_shapeControlsContainer = nullptr;
    m_highlightColorEditor = {};
    m_spotlightColorEditor = {};
    m_shapeButtonGroup = nullptr;
    m_rectangleShapeButton = nullptr;
    m_ellipseShapeButton = nullptr;
    m_diamondShapeButton = nullptr;
    m_highlightStrokeEditor = {};
    m_penHighlightColorEditor = {};
    m_penHighlightStrokeWidthEditor = {};
    m_penFilterStrokeWidthEditor = {};
    m_arrowStrokeWidthEditor = {};
    m_arrowStrokeEditor = {};
    m_arrowTypeControlsContainer = nullptr;
    m_arrowTypeButtonGroup = nullptr;
    m_startArrowheadEditor = {};
    m_endArrowheadEditor = {};
    m_textColorEditor = {};
    m_textFontEditor = {};
    m_textStrokeEditor = {};
    m_textFillEditor = {};
    m_textCornerRadiusEditor = nullptr;
    m_textAlignmentEditor = {};
    m_serialNumberColorEditor = {};
    m_serialNumberFillEditor = {};
    m_serialNumberEditor = nullptr;
    m_serialNumberFontEditor = {};
    m_watermarkColorPreviewPending = false;
    m_watermarkColorEditor = {};
    m_watermarkTextEdit = nullptr;
    m_watermarkFontEditor = {};
    m_watermarkAngleEditor = nullptr;
    m_watermarkGapEditor = nullptr;
    m_watermarkOpacityEditor = {};
    m_toolbarSpacingItems.clear();
}

void ScreenshotToolPaletteStyleControls::setCreationStyleDefaults(
    const SnowCanvasStyleDefaults& defaults) {
    const SnowCanvasWatermarkConfig watermark = m_watermarkConfig;
    const SnowCanvasSpotlightConfig spotlight = spotlightConfig;
    ScreenshotToolPaletteStyleState::reset(defaults);
    m_watermarkConfig = watermark;
    spotlightConfig = spotlight;
    updateRectangleStyleControls();
    updateArrowStyleControls();
    updateHighlightStyleControls();
    updatePenHighlightStyleControls();
    updateTextStyleControls();
    updateSerialNumberStyleControls();
}

bool ScreenshotToolPaletteStyleControls::stepStrokeWidth(int direction) {
    if (m_penHighlightControlsActive) {
        if (direction == 0) {
            return false;
        }
        setPenHighlightStrokeWidth(m_penHighlightStyle.strokeWidth + (direction > 0 ? 1.0 : -1.0));
        return true;
    }

    if (m_arrowControlsActive) {
        if (direction == 0) {
            return false;
        }

        const double strokeWidth =
            std::clamp(m_arrowStyle.strokeWidth + (direction > 0 ? 1.0 : -1.0), 1.0, 72.0);
        if (qFuzzyCompare(strokeWidth + 1.0, m_arrowStyle.strokeWidth + 1.0) &&
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

void ScreenshotToolPaletteStyleControls::setArrowControlsActive(bool active) {
    if (m_arrowControlsActive == active) {
        return;
    }
    m_arrowControlsActive = active;
}

void ScreenshotToolPaletteStyleControls::setLineControlsActive(bool active) {
    if (m_lineControlsActive == active) {
        return;
    }
    m_lineControlsActive = active;
    updateRectangleOnlyControlsVisibility();
    if (active) {
        updateRectangleStyleControls();
    }
}

void ScreenshotToolPaletteStyleControls::setFreeDrawControlsActive(bool active) {
    if (m_freeDrawControlsActive == active) {
        return;
    }
    m_freeDrawControlsActive = active;
    updateRectangleOnlyControlsVisibility();
    if (active) {
        updateRectangleStyleControls();
    }
}

void ScreenshotToolPaletteStyleControls::setHighlightControlsActive(bool active) {
    if (m_highlightControlsActive == active) {
        return;
    }
    m_highlightControlsActive = active;
    updateHighlightStyleControls();
}

void ScreenshotToolPaletteStyleControls::setPenHighlightControlsActive(bool active) {
    if (m_penHighlightControlsActive == active) {
        return;
    }
    m_penHighlightControlsActive = active;
    updatePenHighlightStyleControls();
}

void ScreenshotToolPaletteStyleControls::setTextControlsActive(bool active) {
    if (m_textControlsActive == active) {
        return;
    }
    if (!active) {
        clearTextStylePopupInteractions();
    }
    m_textControlsActive = active;
}

void ScreenshotToolPaletteStyleControls::observeTextStylePopup(
    adqt::widgets::AdColorPicker* picker) {
    if (picker == nullptr) {
        return;
    }
    QObject::connect(picker, &adqt::widgets::AdColorPicker::popupOpening, picker,
                     [this, picker]() { beginTextStylePopupInteraction(picker); });
    QObject::connect(picker, &adqt::widgets::AdColorPicker::popupVisibleChanged, picker,
                     [this, picker](bool visible) {
                         if (!visible) {
                             endTextStylePopupInteraction(picker);
                         }
                     });
}

void ScreenshotToolPaletteStyleControls::observeTextStylePopup(adqt::widgets::AdSelect* select) {
    if (select == nullptr) {
        return;
    }
    QObject::connect(select, &adqt::widgets::AdSelect::popupOpening, select,
                     [this, select]() { beginTextStylePopupInteraction(select); });
    QObject::connect(select, &adqt::widgets::AdSelect::popupVisibleChanged, select,
                     [this, select](bool visible) {
                         if (!visible) {
                             endTextStylePopupInteraction(select);
                         }
                     });
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

bool ScreenshotToolPaletteStyleControls::stepTextFontSize(int direction) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedFontSize) != 0;
    if (!m_textStyle.stepFontSize(direction) && !wasMixed) {
        return false;
    }
    static_cast<void>(m_creationTextStyle.setFontSize(m_textStyle.textStyle().fontSize));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedFontSize;
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

    setCornerRadius(m_rectangleStyle.cornerRadius() + (direction > 0 ? 1 : -1));
    return true;
}

bool ScreenshotToolPaletteStyleControls::handleTextStrokeWidthWheel(const QPoint& globalPosition,
                                                                    int direction) {
    if (direction == 0 || m_textStrokeEditor.picker == nullptr ||
        !m_textStrokeEditor.picker->isVisible() ||
        !m_textStrokeEditor.picker->rect().contains(
            m_textStrokeEditor.picker->mapFromGlobal(globalPosition))) {
        return false;
    }
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedStrokeWidth) != 0;
    if (m_textStyle.stepStrokeWidth(direction) || wasMixed) {
        static_cast<void>(m_creationTextStyle.setStrokeWidth(m_textStyle.textStyle().strokeWidth));
        m_textStyleMixed &= ~SnowCanvasTextStyleMixedStrokeWidth;
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
    setTextCornerRadius(qRound(m_textStyle.textStyle().cornerRadii.topLeft) +
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
            direction > 0 ? (m_serialNumberStyle.number < std::numeric_limits<qint64>::max()
                                 ? m_serialNumberStyle.number + 1
                                 : m_serialNumberStyle.number)
                          : std::max<qint64>(0, m_serialNumberStyle.number - 1);
        setSerialNumber(nextNumber);
        return true;
    }
    if (m_serialNumberFontEditor.sizeSummary != nullptr &&
        m_serialNumberFontEditor.sizeSummary->rect().contains(
            m_serialNumberFontEditor.sizeSummary->mapFromGlobal(globalPosition))) {
        setSerialNumberFontSize(m_serialNumberStyle.fontSize + (direction > 0 ? 1.0 : -1.0));
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
                                 static_cast<double>(qRound(m_watermarkConfig.angle)) +
                                     (direction > 0 ? 1.0 : -1.0),
                                 90.0));
        return true;
    }
    if (m_watermarkGapEditor != nullptr && m_watermarkGapEditor->isVisible() &&
        m_watermarkGapEditor->rect().contains(
            m_watermarkGapEditor->mapFromGlobal(globalPosition))) {
        setWatermarkGap(qBound(
            10.0, static_cast<double>(qRound(m_watermarkConfig.gap)) + (direction > 0 ? 1.0 : -1.0),
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
    if (direction == 0 || !std::isfinite(m_watermarkConfig.fontSize)) {
        return false;
    }

    setWatermarkFontSize(std::clamp(m_watermarkConfig.fontSize + (direction > 0 ? 1.0 : -1.0),
                                    kMinWatermarkFontSize, kMaxWatermarkFontSize));
    return true;
}

SnowCanvasShapeStyle ScreenshotToolPaletteStyleControls::rectangleStyle() const {
    return m_rectangleStyle.rectangleStyle();
}

SnowCanvasArrowStyle ScreenshotToolPaletteStyleControls::arrowStyle() const {
    return m_arrowStyle;
}

ScreenshotToolPaletteRectangleStyleModel& ScreenshotToolPaletteStyleControls::activeShapeStyle() {
    return m_highlightControlsActive  ? m_highlightStyle
           : m_freeDrawControlsActive ? m_freeDrawStyle
           : m_lineControlsActive     ? m_lineStyle
                                      : m_rectangleStyle;
}

const ScreenshotToolPaletteRectangleStyleModel&
ScreenshotToolPaletteStyleControls::activeShapeStyle() const {
    return m_highlightControlsActive  ? m_highlightStyle
           : m_freeDrawControlsActive ? m_freeDrawStyle
           : m_lineControlsActive     ? m_lineStyle
                                      : m_rectangleStyle;
}

ScreenshotToolPaletteRectangleStyleModel&
ScreenshotToolPaletteStyleControls::activeCreationShapeStyle() {
    return m_highlightControlsActive  ? m_creationHighlightStyle
           : m_freeDrawControlsActive ? m_creationFreeDrawStyle
           : m_lineControlsActive     ? m_creationLineStyle
                                      : m_creationRectangleStyle;
}

SnowCanvasShapeKind ScreenshotToolPaletteStyleControls::activeShapeKind() const {
    return m_highlightControlsActive  ? SnowCanvasShapeKind::RectangleHighlight
           : m_freeDrawControlsActive ? SnowCanvasShapeKind::FreeDraw
           : m_lineControlsActive     ? SnowCanvasShapeKind::Line
                                      : SnowCanvasShapeKind::Rectangle;
}

SnowCanvasTextStyle ScreenshotToolPaletteStyleControls::textStyle() const {
    return m_textStyle.textStyle();
}

SnowCanvasStyleDefaults ScreenshotToolPaletteStyleControls::creationStyleDefaults() const {
    SnowCanvasStyleDefaults defaults = m_defaults;
    defaults.rectangle = m_creationRectangleStyle.rectangleStyle();
    defaults.line = m_creationLineStyle.rectangleStyle();
    defaults.freeDraw = m_creationFreeDrawStyle.rectangleStyle();
    defaults.rectangleHighlight = m_creationHighlightStyle.rectangleStyle();
    defaults.penHighlight = m_creationPenHighlightStyle;
    defaults.arrow.stroke = m_creationArrowStyle.stroke;
    defaults.arrow.strokeWidth = m_creationArrowStyle.strokeWidth;
    defaults.arrow.startArrowhead = m_creationArrowStyle.startArrowhead;
    defaults.arrow.endArrowhead = m_creationArrowStyle.endArrowhead;
    defaults.arrow.strokeStyle = m_creationArrowStyle.strokeStyle;
    defaults.arrow.arrowType = m_creationArrowStyle.arrowType;
    defaults.text = m_creationTextStyle.textStyle();
    defaults.serialNumber = m_creationSerialNumberStyle;
    defaults.rectangleFilter = creationRectangleFilterStyle;
    defaults.penFilter = creationPenFilterStyle;
    defaults.watermark = m_watermarkConfig;
    defaults.spotlight = spotlightConfig;
    return defaults;
}

void ScreenshotToolPaletteStyleControls::setRectangleStyle(const SnowCanvasShapeStyle& style) {
    m_creationRectangleStyle.setRectangleStyle(style);
    m_rectangleStyle = m_creationRectangleStyle;
    m_showingSelectedStyle = false;
    m_selectedStyleMixed = 0;
    updateRectangleStyleControls();
}

void ScreenshotToolPaletteStyleControls::setWatermarkConfig(
    const SnowCanvasWatermarkConfig& config) {
    m_watermarkColorPreviewPending = false;
    if (m_watermarkConfig == config) {
        return;
    }
    m_watermarkConfig = config;
    updateWatermarkControls();
}

void ScreenshotToolPaletteStyleControls::setStyleToolbarState(
    const SnowCanvasStyleToolbarState& state) {
    const bool serialNumberStyleSource =
        state.source == SnowCanvasStyleToolbarSource::DefaultSerialNumber ||
        state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;
    if (serialNumberStyleSource) {
        const bool selectedSerialNumber =
            state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;
        SnowCanvasSerialNumberStyle displayedStyle = state.serialNumberStyle;
        if (m_handlingSerialNumberColorPickerChange) {
            displayedStyle.color = m_serialNumberStyle.color;
        }
        if (m_handlingSerialNumberFillColorPickerChange) {
            displayedStyle.fill = m_serialNumberStyle.fill;
        }
        const quint32 mixed = selectedSerialNumber ? state.serialNumberStyleMixed : 0;
        if (m_styleSource == state.source && m_serialNumberStyle == displayedStyle &&
            m_serialNumberStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_serialNumberStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_styleSource != state.source) {
            groups = AllSerialNumberRefreshes;
        } else {
            if (m_serialNumberStyle.number != displayedStyle.number ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedNumber) != 0)
                groups |= SerialNumberValueRefresh;
            if (m_serialNumberStyle.color != displayedStyle.color ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedColor) != 0)
                groups |= SerialNumberColorRefresh;
            if (m_serialNumberStyle.fill != displayedStyle.fill ||
                m_serialNumberStyle.fillStyle != displayedStyle.fillStyle ||
                (mixedChanged & (SnowCanvasSerialNumberStyleMixedFill |
                                 SnowCanvasSerialNumberStyleMixedFillStyle)) != 0)
                groups |= SerialNumberFillRefresh;
            if (m_serialNumberStyle.fontSize != displayedStyle.fontSize ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedFontSize) != 0)
                groups |= SerialNumberFontSizeRefresh;
            if (m_serialNumberStyle.fontFamily != displayedStyle.fontFamily ||
                (mixedChanged & SnowCanvasSerialNumberStyleMixedFontFamily) != 0)
                groups |= SerialNumberFontFamilyRefresh;
        }
        m_styleSource = state.source;
        m_serialNumberStyleMixed = mixed;
        m_serialNumberStyle = displayedStyle;
        if (!selectedSerialNumber) {
            m_creationSerialNumberStyle = displayedStyle;
        }
        updateSerialNumberStyleControls(groups);
        return;
    }

    const bool textStyleSource = state.source == SnowCanvasStyleToolbarSource::DefaultText ||
                                 state.source == SnowCanvasStyleToolbarSource::SelectedText;
    if (textStyleSource) {
        const bool selectedText = state.source == SnowCanvasStyleToolbarSource::SelectedText;
        SnowCanvasTextStyle displayedStyle = state.textStyle;
        if (m_handlingTextColorPickerChange) {
            displayedStyle.color = m_textStyle.textStyle().color;
        }
        if (m_handlingTextStrokeColorPickerChange) {
            displayedStyle.stroke = m_textStyle.textStyle().stroke;
        }
        if (m_handlingTextFillColorPickerChange) {
            displayedStyle.fill = m_textStyle.textStyle().fill;
        }
        const quint32 mixed = selectedText ? state.textStyleMixed : 0;
        const SnowCanvasTextStyle previous = m_textStyle.textStyle();
        m_textStyle.setTextStyle(displayedStyle);
        const SnowCanvasTextStyle normalized = m_textStyle.textStyle();
        if (m_styleSource == state.source && previous == normalized && m_textStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_textStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_styleSource != state.source) {
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
        m_styleSource = state.source;
        m_showingSelectedTextStyle = selectedText;
        m_textStyleMixed = mixed;
        if (!selectedText) {
            m_creationTextStyle.setTextStyle(normalized);
        }
        updateTextStyleControls(groups);
        return;
    }

    const bool arrowStyleSource = state.source == SnowCanvasStyleToolbarSource::DefaultArrow ||
                                  state.source == SnowCanvasStyleToolbarSource::SelectedArrow;
    if (arrowStyleSource) {
        const bool selectedArrow = state.source == SnowCanvasStyleToolbarSource::SelectedArrow;
        SnowCanvasArrowStyle displayedStyle = arrowStyleFromShapeStyle(state.shapeStyle);
        if (m_handlingArrowStrokeColorPickerChange) {
            displayedStyle.stroke = m_arrowStyle.stroke;
        }
        const quint32 mixed = selectedArrow ? state.shapeStyleMixed : 0;
        if (m_styleSource == state.source && m_arrowStyle == displayedStyle &&
            m_selectedStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_selectedStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_styleSource != state.source) {
            groups = AllShapeRefreshes;
        } else {
            if (m_arrowStyle.strokeWidth != displayedStyle.strokeWidth ||
                (mixedChanged & SnowCanvasShapeStyleMixedStrokeWidth) != 0)
                groups |= ShapeStrokeWidthRefresh;
            if (m_arrowStyle.stroke != displayedStyle.stroke ||
                m_arrowStyle.strokeStyle != displayedStyle.strokeStyle ||
                (mixedChanged &
                 (SnowCanvasShapeStyleMixedStroke | SnowCanvasShapeStyleMixedStrokeStyle)) != 0)
                groups |= ShapeStrokeRefresh;
            if (m_arrowStyle.arrowType != displayedStyle.arrowType ||
                (mixedChanged & SnowCanvasShapeStyleMixedArrowType) != 0)
                groups |= ShapeArrowTypeRefresh;
            if (m_arrowStyle.startArrowhead != displayedStyle.startArrowhead ||
                m_arrowStyle.endArrowhead != displayedStyle.endArrowhead ||
                (mixedChanged & (SnowCanvasShapeStyleMixedStartArrowhead |
                                 SnowCanvasShapeStyleMixedEndArrowhead)) != 0)
                groups |= ShapeArrowheadsRefresh;
        }
        m_styleSource = state.source;
        m_showingSelectedStyle = selectedArrow;
        m_selectedStyleMixed = mixed;
        m_arrowStyle = displayedStyle;
        if (!selectedArrow) {
            m_creationArrowStyle = displayedStyle;
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
        if (m_handlingPenHighlightColorPickerChange) {
            displayedStyle.stroke = m_penHighlightStyle.stroke;
        }
        const quint32 mixed = selectedPenHighlight ? state.shapeStyleMixed : 0;
        if (m_styleSource == state.source && m_penHighlightStyle == displayedStyle &&
            m_selectedStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        const quint32 mixedChanged = m_selectedStyleMixed ^ mixed;
        quint32 groups = 0;
        if (m_styleSource != state.source)
            groups = AllShapeRefreshes;
        else {
            if (m_penHighlightStyle.stroke != displayedStyle.stroke ||
                (mixedChanged & SnowCanvasShapeStyleMixedStroke) != 0)
                groups |= ShapeStrokeRefresh;
            if (m_penHighlightStyle.strokeWidth != displayedStyle.strokeWidth ||
                (mixedChanged & SnowCanvasShapeStyleMixedStrokeWidth) != 0)
                groups |= ShapeStrokeWidthRefresh;
        }
        m_styleSource = state.source;
        m_showingSelectedStyle = selectedPenHighlight;
        m_selectedStyleMixed = mixed;
        m_penHighlightStyle = displayedStyle;
        if (!selectedPenHighlight) {
            m_creationPenHighlightStyle = displayedStyle;
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
    auto& displayedModel = highlightStyleSource  ? m_highlightStyle
                           : freeDrawStyleSource ? m_freeDrawStyle
                           : lineStyleSource     ? m_lineStyle
                                                 : m_rectangleStyle;
    auto& creationModel = highlightStyleSource  ? m_creationHighlightStyle
                          : freeDrawStyleSource ? m_creationFreeDrawStyle
                          : lineStyleSource     ? m_creationLineStyle
                                                : m_creationRectangleStyle;
    SnowCanvasShapeStyle displayedStyle = state.shapeStyle;
    if (m_handlingStrokeColorPickerChange) {
        displayedStyle.stroke = displayedModel.strokeColor();
    }
    if (highlightStyleSource && m_handlingHighlightColorPickerChange) {
        displayedStyle.fill = displayedModel.fillColor();
    }
    if (highlightStyleSource && m_handlingHighlightStrokeColorPickerChange) {
        displayedStyle.stroke = displayedModel.strokeColor();
    }
    const quint32 mixed = selectedShape ? state.shapeStyleMixed : 0;
    if (m_styleSource == state.source && displayedModel.rectangleStyle() == displayedStyle &&
        m_selectedStyleMixed == mixed) {
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_styleStateNoopCount;
#endif
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
        return;
    }
    m_lineControlsActive = lineStyleSource;
    m_freeDrawControlsActive = freeDrawStyleSource;
    m_highlightControlsActive = highlightStyleSource;
    m_showingSelectedStyle = selectedShape;
    updateRectangleOnlyControlsVisibility();
    const SnowCanvasShapeStyle previous = displayedModel.rectangleStyle();
    const quint32 mixedChanged = m_selectedStyleMixed ^ mixed;
    quint32 groups = 0;
    if (m_styleSource != state.source)
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
    m_styleSource = state.source;
    if (m_showingSelectedStyle) {
        displayedModel.setRectangleStyle(displayedStyle);
        m_selectedStyleMixed = mixed;
    } else {
        creationModel.setRectangleStyle(displayedStyle);
        displayedModel.setRectangleStyle(displayedStyle);
        m_selectedStyleMixed = 0;
    }
    updateRectangleStyleControls(groups);
}

void ScreenshotToolPaletteStyleControls::setSerialNumberControlsVisible(bool visible) {
    Q_UNUSED(visible);
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

    refreshNumericPresetEditorMetrics(m_shapeStrokeWidthEditor, metrics);
    refreshStrokeEditorMetrics(m_shapeStrokeEditor, metrics);
    refreshFillEditorMetrics(m_shapeFillEditor, metrics);
    refreshColorEditorMetrics(m_highlightColorEditor, metrics);
    refreshColorEditorMetrics(m_spotlightColorEditor, metrics);
    refreshWidthColorEditorMetrics(m_highlightStrokeEditor, metrics);
    refreshColorEditorMetrics(m_penHighlightColorEditor, metrics);
    refreshNumericPresetEditorMetrics(m_penHighlightStrokeWidthEditor, metrics);
    refreshNumericPresetEditorMetrics(m_penFilterStrokeWidthEditor, metrics);
    refreshNumericPresetEditorMetrics(m_arrowStrokeWidthEditor, metrics);
    refreshIconOptionEditorMetrics(m_startArrowheadEditor, metrics);
    refreshIconOptionEditorMetrics(m_endArrowheadEditor, metrics);
    refreshStrokeEditorMetrics(m_arrowStrokeEditor, metrics);
    configureScreenshotToolPaletteCornerRadiusEditor(m_cornerRadiusEditor, metrics);

    refreshColorEditorMetrics(m_textColorEditor, metrics);
    refreshFontEditorMetrics(m_textFontEditor, metrics);
    refreshWidthColorEditorMetrics(m_textStrokeEditor, metrics);
    refreshFillEditorMetrics(m_textFillEditor, metrics);
    configureScreenshotToolPaletteCornerRadiusEditor(m_textCornerRadiusEditor, metrics);
    refreshIconOptionEditorMetrics(m_textAlignmentEditor, metrics);

    refreshColorEditorMetrics(m_watermarkColorEditor, metrics);
    if (applies(m_watermarkTextEdit)) {
        m_watermarkTextEdit->setFixedSize(
            qMax(1, qRound(135.0 * metrics.physicalScale)),
            qMax(1, qRound(metrics.buttonSize * metrics.physicalScale)));
    }
    refreshFontEditorMetrics(m_watermarkFontEditor, metrics);
    configureScreenshotToolPaletteIconNumericValueButton(m_watermarkAngleEditor, metrics);
    configureScreenshotToolPaletteIconNumericValueButton(m_watermarkGapEditor, metrics);
    refreshWatermarkOpacityMetrics(metrics);

    refreshColorEditorMetrics(m_serialNumberColorEditor, metrics);
    refreshFillEditorMetrics(m_serialNumberFillEditor, metrics);
    configureScreenshotToolPaletteCornerRadiusEditor(m_serialNumberEditor, metrics);
    refreshFontEditorMetrics(m_serialNumberFontEditor, metrics);

    const auto resetPopupContent = [](QWidget* content) {
        if (content == nullptr) {
            return;
        }
        for (adqt::widgets::AdButton* button : content->findChildren<adqt::widgets::AdButton*>()) {
            resetPopupButtonControlScale(button);
        }
    };
    for (adqt::widgets::AdColorPicker* picker : {
             m_shapeStrokeEditor.picker,
             m_shapeFillEditor.picker,
             m_highlightColorEditor.picker,
             m_spotlightColorEditor.picker,
             m_highlightStrokeEditor.picker,
             m_arrowStrokeEditor.picker,
             m_textColorEditor.picker,
             m_textStrokeEditor.picker,
             m_textFillEditor.picker,
             m_serialNumberColorEditor.picker,
             m_serialNumberFillEditor.picker,
             m_watermarkColorEditor.picker,
         }) {
        if (picker != nullptr) {
            resetPopupContent(picker->popupContent());
            if (applies(picker)) {
                activateWidgetLayoutTree(picker);
            }
        }
    }
}

void ScreenshotToolPaletteStyleControls::refreshThemeIcons(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshWatermarkOpacityMetrics(metrics);
}

void ScreenshotToolPaletteStyleControls::setStrokeWidth(double strokeWidth) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!style.setStrokeWidth(strokeWidth) && !wasMixed) {
        return;
    }

    static_cast<void>(creationStyle.setStrokeWidth(style.strokeWidth()));
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeWidth);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyStrokeWidth,
                            activeShapeKind());
}

void ScreenshotToolPaletteStyleControls::cycleStrokeWidth() {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!style.cycleStrokeWidth() && !wasMixed) {
        return;
    }

    static_cast<void>(creationStyle.setStrokeWidth(style.strokeWidth()));
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeWidth);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyStrokeWidth,
                            activeShapeKind());
}

void ScreenshotToolPaletteStyleControls::setStrokeColor(const QColor& color) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!style.setStrokeColor(color) && !wasMixed) {
        return;
    }

    static_cast<void>(creationStyle.setStrokeColor(style.strokeColor()));
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeColor);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyStrokeColor,
                            activeShapeKind());
}

void ScreenshotToolPaletteStyleControls::setStrokeStyle(SnowCanvasStrokeStyle strokeStyle) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeStyle);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!style.setStrokeStyle(strokeStyle) && !wasMixed) {
        return;
    }

    static_cast<void>(creationStyle.setStrokeStyle(style.strokeStyle()));
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeStyle);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyStrokeStyle,
                            activeShapeKind());
}

void ScreenshotToolPaletteStyleControls::setFillColor(const QColor& color) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillColor);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!style.setFillColor(color) && !wasMixed) {
        return;
    }

    static_cast<void>(creationStyle.setFillColor(style.fillColor()));
    clearMixedProperties(SnowCanvasShapeStylePropertyFillColor);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyFillColor,
                            activeShapeKind());
}

void ScreenshotToolPaletteStyleControls::setFillStyle(SnowCanvasFillStyle fillStyle) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillStyle);
    auto& style = activeShapeStyle();
    auto& creationStyle = activeCreationShapeStyle();
    if (!style.setFillStyle(fillStyle) && !wasMixed) {
        return;
    }

    static_cast<void>(creationStyle.setFillStyle(style.fillStyle()));
    clearMixedProperties(SnowCanvasShapeStylePropertyFillStyle);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(style.rectangleStyle(), SnowCanvasShapeStylePropertyFillStyle,
                            activeShapeKind());
}

void ScreenshotToolPaletteStyleControls::setCornerRadius(int cornerRadius) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyCornerRadius);
    if (!m_rectangleStyle.setCornerRadius(cornerRadius) && !wasMixed) {
        return;
    }

    static_cast<void>(m_creationRectangleStyle.setCornerRadius(m_rectangleStyle.cornerRadius()));
    clearMixedProperties(SnowCanvasShapeStylePropertyCornerRadius);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(m_rectangleStyle.rectangleStyle(),
                            SnowCanvasShapeStylePropertyCornerRadius,
                            SnowCanvasShapeKind::Rectangle);
}

void ScreenshotToolPaletteStyleControls::setPenHighlightColor(const QColor& color) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    if (!color.isValid() || (m_penHighlightStyle.stroke == color && !wasMixed)) {
        return;
    }

    m_penHighlightStyle.stroke = color;
    m_penHighlightStyle.stroke.setAlpha(255);
    m_creationPenHighlightStyle.stroke = m_penHighlightStyle.stroke;
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeColor);
    updatePenHighlightStyleControls();
    notifyShapeStyleChanged(m_penHighlightStyle, SnowCanvasShapeStylePropertyStrokeColor,
                            SnowCanvasShapeKind::PenHighlight);
}

void ScreenshotToolPaletteStyleControls::setPenHighlightStrokeWidth(double strokeWidth) {
    const double clampedStrokeWidth = std::clamp(strokeWidth, 1.0, 72.0);
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    if (qFuzzyCompare(m_penHighlightStyle.strokeWidth + 1.0, clampedStrokeWidth + 1.0) &&
        !wasMixed) {
        return;
    }

    m_penHighlightStyle.strokeWidth = clampedStrokeWidth;
    m_creationPenHighlightStyle.strokeWidth = clampedStrokeWidth;
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeWidth);
    updatePenHighlightStyleControls();
    notifyShapeStyleChanged(m_penHighlightStyle, SnowCanvasShapeStylePropertyStrokeWidth,
                            SnowCanvasShapeKind::PenHighlight);
}

void ScreenshotToolPaletteStyleControls::setArrowStrokeWidth(double strokeWidth) {
    const double clampedStrokeWidth = std::clamp(strokeWidth, 1.0, 72.0);
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    if (qFuzzyCompare(m_arrowStyle.strokeWidth + 1.0, clampedStrokeWidth + 1.0) && !wasMixed) {
        return;
    }

    m_arrowStyle.strokeWidth = clampedStrokeWidth;
    m_creationArrowStyle.strokeWidth = clampedStrokeWidth;
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeWidth);
    updateArrowStyleControls();
    notifyShapeStyleChanged(shapeStyleFromArrowStyle(m_arrowStyle),
                            SnowCanvasShapeStylePropertyStrokeWidth, SnowCanvasShapeKind::Arrow);
}

void ScreenshotToolPaletteStyleControls::cycleArrowStrokeWidth() {
    const QVector<double>& values = arrowStrokeWidthValues();
    for (double value : values) {
        if (value > m_arrowStyle.strokeWidth) {
            setArrowStrokeWidth(value);
            return;
        }
    }

    if (!values.isEmpty()) {
        setArrowStrokeWidth(values.first());
    }
}

void ScreenshotToolPaletteStyleControls::setArrowStrokeColor(const QColor& color) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    if (!color.isValid() || (m_arrowStyle.stroke == color && !wasMixed)) {
        return;
    }

    m_arrowStyle.stroke = color;
    m_arrowStyle.stroke.setAlpha(255);
    m_creationArrowStyle.stroke = m_arrowStyle.stroke;
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeColor);
    updateArrowStyleControls();
    notifyShapeStyleChanged(shapeStyleFromArrowStyle(m_arrowStyle),
                            SnowCanvasShapeStylePropertyStrokeColor, SnowCanvasShapeKind::Arrow);
}

void ScreenshotToolPaletteStyleControls::setArrowStrokeStyle(
    SnowCanvasStrokeStyle strokeStyle) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeStyle);
    if (m_arrowStyle.strokeStyle == strokeStyle && !wasMixed) {
        return;
    }

    m_arrowStyle.strokeStyle = strokeStyle;
    m_creationArrowStyle.strokeStyle = strokeStyle;
    clearMixedProperties(SnowCanvasShapeStylePropertyStrokeStyle);
    updateArrowStyleControls();
    notifyShapeStyleChanged(shapeStyleFromArrowStyle(m_arrowStyle),
                            SnowCanvasShapeStylePropertyStrokeStyle, SnowCanvasShapeKind::Arrow);
}

void ScreenshotToolPaletteStyleControls::setArrowType(SnowCanvasArrowType arrowType) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyArrowType);
    if (m_arrowStyle.arrowType == arrowType && !wasMixed) {
        return;
    }

    m_arrowStyle.arrowType = arrowType;
    m_creationArrowStyle.arrowType = arrowType;
    clearMixedProperties(SnowCanvasShapeStylePropertyArrowType);
    updateArrowStyleControls();
    notifyShapeStyleChanged(shapeStyleFromArrowStyle(m_arrowStyle),
                            SnowCanvasShapeStylePropertyArrowType, SnowCanvasShapeKind::Arrow);
}

void ScreenshotToolPaletteStyleControls::setArrowhead(bool start, SnowCanvasArrowhead arrowhead) {
    SnowCanvasArrowhead& currentArrowhead =
        start ? m_arrowStyle.startArrowhead : m_arrowStyle.endArrowhead;
    const quint32 property = start ? SnowCanvasShapeStylePropertyStartArrowhead
                                   : SnowCanvasShapeStylePropertyEndArrowhead;
    const bool wasMixed = hasMixedProperty(property);
    if (currentArrowhead == arrowhead && !wasMixed) {
        return;
    }

    currentArrowhead = arrowhead;
    SnowCanvasArrowhead& creationArrowhead =
        start ? m_creationArrowStyle.startArrowhead : m_creationArrowStyle.endArrowhead;
    creationArrowhead = arrowhead;
    clearMixedProperties(property);
    updateArrowStyleControls();
    notifyShapeStyleChanged(shapeStyleFromArrowStyle(m_arrowStyle), property,
                            SnowCanvasShapeKind::Arrow);
}

void ScreenshotToolPaletteStyleControls::updateArrowStyleControls(quint32 groups) {
#if defined(SNOW_SHOT_TEST_HOOKS)
    m_propertyGroupRefreshCount += propertyGroupCount(groups, AllShapeRefreshes);
#endif
    if (groups != 0xffffffffu) {
        const auto mixed = [this](quint32 property) { return hasMixedProperty(property); };
        if ((groups & ShapeStrokeWidthRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.stroke_width_refresh");
            const bool isMixed = mixed(SnowCanvasShapeStylePropertyStrokeWidth);
            updateNumericPresetEditor(m_arrowStrokeWidthEditor, m_arrowStyle.strokeWidth, isMixed);
        }
        if ((groups & ShapeStrokeRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.stroke_refresh");
            const bool colorMixed = mixed(SnowCanvasShapeStylePropertyStrokeColor);
            const bool styleMixed = mixed(SnowCanvasShapeStylePropertyStrokeStyle);
            updateStrokeEditor(m_arrowStrokeEditor, m_arrowStyle.stroke, m_arrowStyle.strokeStyle,
                               colorMixed, styleMixed);
        }
        if ((groups & ShapeArrowTypeRefresh) != 0 && m_arrowTypeButtonGroup != nullptr) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.type_refresh");
            const QSignalBlocker blocker(m_arrowTypeButtonGroup);
            m_arrowTypeButtonGroup->setCheckedId(
                mixed(SnowCanvasShapeStylePropertyArrowType)           ? -1
                : m_arrowStyle.arrowType == SnowCanvasArrowType::Curve ? 1
                : m_arrowStyle.arrowType == SnowCanvasArrowType::Elbow ? 2
                                                                       : 0);
        }
        if ((groups & ShapeArrowheadsRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.arrowheads_refresh");
            const bool startMixed = mixed(SnowCanvasShapeStylePropertyStartArrowhead);
            const bool endMixed = mixed(SnowCanvasShapeStylePropertyEndArrowhead);
            updateIconOptionEditor(m_startArrowheadEditor,
                                   static_cast<int>(m_arrowStyle.startArrowhead), startMixed);
            updateIconOptionEditor(m_endArrowheadEditor,
                                   static_cast<int>(m_arrowStyle.endArrowhead), endMixed);
        }
        return;
    }
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.arrow.property_group_refresh");
    const bool strokeWidthMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    const bool strokeColorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    const bool strokeStyleMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeStyle);
    const bool arrowTypeMixed = hasMixedProperty(SnowCanvasShapeStylePropertyArrowType);
    const bool startArrowheadMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStartArrowhead);
    const bool endArrowheadMixed = hasMixedProperty(SnowCanvasShapeStylePropertyEndArrowhead);

    updateNumericPresetEditor(m_arrowStrokeWidthEditor, m_arrowStyle.strokeWidth, strokeWidthMixed);

    updateStrokeEditor(m_arrowStrokeEditor, m_arrowStyle.stroke, m_arrowStyle.strokeStyle,
                       strokeColorMixed, strokeStyleMixed);

    if (m_arrowTypeButtonGroup != nullptr) {
        const QSignalBlocker blocker(m_arrowTypeButtonGroup);
        m_arrowTypeButtonGroup->setCheckedId(
            arrowTypeMixed                                         ? -1
            : m_arrowStyle.arrowType == SnowCanvasArrowType::Curve ? 1
            : m_arrowStyle.arrowType == SnowCanvasArrowType::Elbow ? 2
                                                                   : 0);
    }

    updateIconOptionEditor(m_startArrowheadEditor, static_cast<int>(m_arrowStyle.startArrowhead),
                           startArrowheadMixed);
    updateIconOptionEditor(m_endArrowheadEditor, static_cast<int>(m_arrowStyle.endArrowhead),
                           endArrowheadMixed);
}

void ScreenshotToolPaletteStyleControls::updateRectangleStyleControls(quint32 groups) {
#if defined(SNOW_SHOT_TEST_HOOKS)
    m_propertyGroupRefreshCount += propertyGroupCount(groups, AllShapeRefreshes);
#endif
    if (groups != 0xffffffffu) {
        const auto& style = activeShapeStyle();
        if ((groups & ShapeModeRefresh) != 0 && m_shapeButtonGroup != nullptr) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.mode_refresh");
            const QSignalBlocker blocker(m_shapeButtonGroup);
            m_shapeButtonGroup->setCheckedId(style.shape() == SnowCanvasRectangleShape::Ellipse ? 1
                                             : style.shape() == SnowCanvasRectangleShape::Diamond
                                                 ? 2
                                                 : 0);
        }
        if ((groups & ShapeStrokeWidthRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.stroke_width_refresh");
            const bool mixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
            updateNumericPresetEditor(m_shapeStrokeWidthEditor, style.strokeWidth(), mixed);
        }
        if ((groups & ShapeStrokeRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.stroke_refresh");
            const bool colorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
            const bool styleMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeStyle);
            updateStrokeEditor(m_shapeStrokeEditor, style.strokeColor(), style.strokeStyle(),
                               colorMixed, styleMixed);
        }
        if ((groups & ShapeFillRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.fill_refresh");
            const bool colorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillColor);
            const bool styleMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillStyle);
            updateFillEditor(m_shapeFillEditor, style.fillColor(), style.fillStyle(), colorMixed,
                             styleMixed);
        }
        if ((groups & ShapeCornerRefresh) != 0 && m_cornerRadiusEditor != nullptr) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.corner_refresh");
            m_cornerRadiusEditor->setCornerRadius(style.cornerRadius());
            m_cornerRadiusEditor->setMixed(
                hasMixedProperty(SnowCanvasShapeStylePropertyCornerRadius));
        }
        if (m_highlightControlsActive) {
            updateHighlightStyleControls(groups);
        }
        return;
    }
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.shape.property_group_refresh");
    const auto& style = activeShapeStyle();
    if (m_shapeButtonGroup != nullptr) {
        const QSignalBlocker blocker(m_shapeButtonGroup);
        const auto shape = style.shape();
        m_shapeButtonGroup->setCheckedId(shape == SnowCanvasRectangleShape::Ellipse   ? 1
                                         : shape == SnowCanvasRectangleShape::Diamond ? 2
                                                                                      : 0);
    }
    const bool strokeWidthMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    const bool strokeColorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    const bool strokeStyleMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeStyle);
    const bool fillColorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillColor);
    const bool fillStyleMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillStyle);
    const bool cornerRadiusMixed = hasMixedProperty(SnowCanvasShapeStylePropertyCornerRadius);
    updateNumericPresetEditor(m_shapeStrokeWidthEditor, style.strokeWidth(), strokeWidthMixed);

    updateStrokeEditor(m_shapeStrokeEditor, style.strokeColor(), style.strokeStyle(),
                       strokeColorMixed, strokeStyleMixed);

    updateFillEditor(m_shapeFillEditor, style.fillColor(), style.fillStyle(), fillColorMixed,
                     fillStyleMixed);

    if (m_cornerRadiusEditor != nullptr) {
        m_cornerRadiusEditor->setCornerRadius(style.cornerRadius());
        m_cornerRadiusEditor->setMixed(cornerRadiusMixed);
    }
    updateHighlightStyleControls();
}

void ScreenshotToolPaletteStyleControls::updateRectangleOnlyControlsVisibility() {
    const bool visible = !m_lineControlsActive && !m_freeDrawControlsActive;
    if (m_shapeControlsContainer != nullptr) {
        m_shapeControlsContainer->setVisible(visible);
    }
    if (m_cornerRadiusEditor != nullptr) {
        m_cornerRadiusEditor->setVisible(visible);
    }
}

void ScreenshotToolPaletteStyleControls::setShape(SnowCanvasRectangleShape shape) {
    const bool wasMixed = hasMixedProperty(SnowCanvasShapeStylePropertyShape);
    if (!m_rectangleStyle.setShape(shape) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationRectangleStyle.setShape(shape));
    clearMixedProperties(SnowCanvasShapeStylePropertyShape);
    updateRectangleStyleControls();
    notifyShapeStyleChanged(m_rectangleStyle.rectangleStyle(), SnowCanvasShapeStylePropertyShape,
                            SnowCanvasShapeKind::Rectangle);
}

void ScreenshotToolPaletteStyleControls::updateHighlightStyleControls(quint32 groups) {
    if (groups != 0xffffffffu) {
        const auto& style = m_highlightStyle;
        if ((groups & ShapeFillRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.highlight.fill_refresh");
            const bool mixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillColor);
            updateColorEditor(m_highlightColorEditor, style.fillColor(), mixed);
        }
        if ((groups & (ShapeStrokeRefresh | ShapeStrokeWidthRefresh)) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.highlight.stroke_refresh");
            const bool widthMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
            const bool colorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
            updateWidthColorEditor(m_highlightStrokeEditor, style.strokeWidth(),
                                   style.strokeColor(), widthMixed, colorMixed);
        }
        return;
    }
    const auto& style = m_highlightStyle;
    const bool colorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyFillColor);
    const bool strokeWidthMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
    const bool strokeColorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    updateColorEditor(m_highlightColorEditor, style.fillColor(), colorMixed);

    updateWidthColorEditor(m_highlightStrokeEditor, style.strokeWidth(), style.strokeColor(),
                           strokeWidthMixed, strokeColorMixed);
}

void ScreenshotToolPaletteStyleControls::updatePenHighlightStyleControls(quint32 groups) {
#if defined(SNOW_SHOT_TEST_HOOKS)
    m_propertyGroupRefreshCount += propertyGroupCount(groups, AllShapeRefreshes);
#endif
    if (groups != 0xffffffffu) {
        if ((groups & ShapeStrokeRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.pen_highlight.color_refresh");
            const bool mixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
            updateColorEditor(m_penHighlightColorEditor, m_penHighlightStyle.stroke, mixed);
        }
        if ((groups & ShapeStrokeWidthRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.pen_highlight.width_refresh");
            const bool mixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);
            updateNumericPresetEditor(m_penHighlightStrokeWidthEditor,
                                      m_penHighlightStyle.strokeWidth, mixed);
        }
        return;
    }
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.pen_highlight.property_group_refresh");
    const bool colorMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeColor);
    const bool strokeWidthMixed = hasMixedProperty(SnowCanvasShapeStylePropertyStrokeWidth);

    updateColorEditor(m_penHighlightColorEditor, m_penHighlightStyle.stroke, colorMixed);

    updateNumericPresetEditor(m_penHighlightStrokeWidthEditor, m_penHighlightStyle.strokeWidth,
                              strokeWidthMixed);
}

void ScreenshotToolPaletteStyleControls::setTextColor(const QColor& color) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedColor) != 0;
    if (!m_textStyle.setColor(color) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setColor(color));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedColor;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setWatermarkColor(const QColor& color) {
    if (!color.isValid() || (m_watermarkConfig.color == color && !m_watermarkColorPreviewPending)) {
        return;
    }
    m_watermarkConfig.color = color;
    m_watermarkColorPreviewPending = false;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::setWatermarkFontSize(double fontSize) {
    if (!std::isfinite(fontSize)) {
        return;
    }
    fontSize = std::clamp(fontSize, kMinWatermarkFontSize, kMaxWatermarkFontSize);
    if (qFuzzyCompare(m_watermarkConfig.fontSize + 1.0, fontSize + 1.0)) {
        return;
    }
    m_watermarkConfig.fontSize = fontSize;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::cycleWatermarkFontSize() {
    const QVector<double>& values = watermarkFontSizeValues();
    for (double value : values) {
        if (value > m_watermarkConfig.fontSize + 0.001) {
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
    if (m_watermarkConfig.fontFamily == normalized) {
        return;
    }
    m_watermarkConfig.fontFamily = normalized;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::setWatermarkAngle(double angle) {
    if (!std::isfinite(angle)) {
        return;
    }
    angle = std::clamp(angle, -90.0, 90.0);
    if (qFuzzyCompare(m_watermarkConfig.angle + 1.0, angle + 1.0)) {
        return;
    }
    m_watermarkConfig.angle = angle;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::setWatermarkGap(double gap) {
    if (!std::isfinite(gap)) {
        return;
    }
    gap = std::clamp(gap, 10.0, 200.0);
    if (qFuzzyCompare(m_watermarkConfig.gap + 1.0, gap + 1.0)) {
        return;
    }
    m_watermarkConfig.gap = gap;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::setWatermarkOpacity(double opacity) {
    if (!std::isfinite(opacity)) {
        return;
    }
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (qFuzzyCompare(m_watermarkConfig.opacity + 1.0, opacity + 1.0)) {
        return;
    }
    m_watermarkConfig.opacity = opacity;
    updateWatermarkControls();
    notifyWatermarkConfigChanged();
}

void ScreenshotToolPaletteStyleControls::updateWatermarkControls() {
    updateColorEditor(m_watermarkColorEditor, m_watermarkConfig.color, false);

    if (m_watermarkTextEdit != nullptr) {
        const QSignalBlocker blocker(m_watermarkTextEdit);
        m_watermarkTextEdit->setText(m_watermarkConfig.text);
    }
    updateFontEditor(m_watermarkFontEditor, m_watermarkConfig.fontSize,
                     m_watermarkConfig.fontFamily.trimmed(), false, false, 0x3u, 0x1u, 0x2u);

    if (m_watermarkAngleEditor != nullptr) {
        m_watermarkAngleEditor->setValue(qBound(-90, qRound(m_watermarkConfig.angle), 90));
    }
    if (m_watermarkGapEditor != nullptr) {
        m_watermarkGapEditor->setValue(qBound(10, qRound(m_watermarkConfig.gap), 200));
    }
    if (m_watermarkOpacityEditor.slider != nullptr) {
        const QSignalBlocker blocker(m_watermarkOpacityEditor.slider);
        m_watermarkOpacityEditor.slider->setValue(
            qRound(std::clamp(m_watermarkConfig.opacity, 0.0, 1.0) * 100.0));
        m_watermarkOpacityEditor.slider->setAccessibleDescription(
            QStringLiteral("%1%").arg(qRound(m_watermarkOpacityEditor.slider->value())));
    }
}

void ScreenshotToolPaletteStyleControls::refreshWatermarkOpacityMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    configureScreenshotToolPaletteSliderEditor(m_watermarkOpacityEditor, metrics);
}

void ScreenshotToolPaletteStyleControls::notifyWatermarkConfigChanged() const {
    if (m_callbacks.watermarkConfigChanged) {
        m_callbacks.watermarkConfigChanged(m_watermarkConfig);
    }
}

void ScreenshotToolPaletteStyleControls::notifyWatermarkPreviewChanged() const {
    if (m_callbacks.watermarkPreviewChanged) {
        m_callbacks.watermarkPreviewChanged(m_watermarkConfig);
    }
}

void ScreenshotToolPaletteStyleControls::setTextFontSize(double fontSize) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedFontSize) != 0;
    if (!m_textStyle.setFontSize(fontSize) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setFontSize(m_textStyle.textStyle().fontSize));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedFontSize;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::cycleTextFontSize() {
    if (m_textStyle.cycleFontSize()) {
        static_cast<void>(m_creationTextStyle.setFontSize(m_textStyle.textStyle().fontSize));
        m_textStyleMixed &= ~SnowCanvasTextStyleMixedFontSize;
        updateTextStyleControls();
        notifyTextStyleChanged();
    }
}

void ScreenshotToolPaletteStyleControls::setTextFontFamily(const QString& fontFamily) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedFontFamily) != 0;
    if (!m_textStyle.setFontFamily(fontFamily) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setFontFamily(fontFamily));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedFontFamily;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setTextStrokeColor(const QColor& color) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedStroke) != 0;
    if (!m_textStyle.setStrokeColor(color) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setStrokeColor(color));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedStroke;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setTextStrokeWidth(double strokeWidth) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedStrokeWidth) != 0;
    if (!m_textStyle.setStrokeWidth(strokeWidth) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setStrokeWidth(m_textStyle.textStyle().strokeWidth));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedStrokeWidth;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setTextFillColor(const QColor& color) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedFill) != 0;
    if (!m_textStyle.setFillColor(color) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setFillColor(color));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedFill;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setTextFillStyle(SnowCanvasFillStyle fillStyle) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedFillStyle) != 0;
    if (!m_textStyle.setFillStyle(fillStyle) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setFillStyle(fillStyle));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedFillStyle;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setTextCornerRadius(int cornerRadius) {
    const bool wasMixed =
        m_showingSelectedTextStyle && (m_textStyleMixed & SnowCanvasTextStyleMixedCornerRadii) != 0;
    if (!m_textStyle.setCornerRadius(cornerRadius) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setCornerRadius(cornerRadius));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedCornerRadii;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setTextHorizontalAlign(
    SnowCanvasTextHorizontalAlign alignment) {
    const bool wasMixed = m_showingSelectedTextStyle &&
                          (m_textStyleMixed & SnowCanvasTextStyleMixedHorizontalAlign) != 0;
    if (!m_textStyle.setHorizontalAlign(alignment) && !wasMixed) {
        return;
    }
    static_cast<void>(m_creationTextStyle.setHorizontalAlign(alignment));
    m_textStyleMixed &= ~SnowCanvasTextStyleMixedHorizontalAlign;
    updateTextStyleControls();
    notifyTextStyleChanged();
}

void ScreenshotToolPaletteStyleControls::updateTextStyleControls(quint32 groups) {
#if defined(SNOW_SHOT_TEST_HOOKS)
    m_propertyGroupRefreshCount += propertyGroupCount(groups, AllTextRefreshes);
#endif
    if (groups != 0xffffffffu) {
        const SnowCanvasTextStyle& style = m_textStyle.textStyle();
        const auto mixed = [this](quint32 property) {
            return m_showingSelectedTextStyle && (m_textStyleMixed & property) != 0;
        };
        if ((groups & TextColorRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.color_refresh");
            updateColorEditor(m_textColorEditor, style.color, mixed(SnowCanvasTextStyleMixedColor));
        }
        if ((groups & TextFontSizeRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.font_size_refresh");
        }
        if ((groups & TextFontFamilyRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.font_family_refresh");
        }
        updateFontEditor(m_textFontEditor, style.fontSize, style.fontFamily,
                         mixed(SnowCanvasTextStyleMixedFontSize),
                         mixed(SnowCanvasTextStyleMixedFontFamily), groups, TextFontSizeRefresh,
                         TextFontFamilyRefresh);
        if ((groups & TextStrokeRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.stroke_refresh");
            updateWidthColorEditor(m_textStrokeEditor, style.strokeWidth, style.stroke,
                                   mixed(SnowCanvasTextStyleMixedStrokeWidth),
                                   mixed(SnowCanvasTextStyleMixedStroke));
        }
        if ((groups & TextFillRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.fill_refresh");
            updateFillEditor(m_textFillEditor, style.fill, style.fillStyle,
                             mixed(SnowCanvasTextStyleMixedFill),
                             mixed(SnowCanvasTextStyleMixedFillStyle));
        }
        if ((groups & TextCornerRefresh) != 0 && m_textCornerRadiusEditor != nullptr) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.corner_refresh");
            const bool uniform = style.cornerRadii.topLeft == style.cornerRadii.topRight &&
                                 style.cornerRadii.topLeft == style.cornerRadii.bottomRight &&
                                 style.cornerRadii.topLeft == style.cornerRadii.bottomLeft;
            m_textCornerRadiusEditor->setCornerRadius(qRound(style.cornerRadii.topLeft));
            m_textCornerRadiusEditor->setMixed(mixed(SnowCanvasTextStyleMixedCornerRadii) ||
                                               !uniform);
        }
        if ((groups & TextAlignmentRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.alignment_refresh");
            updateIconOptionEditor(m_textAlignmentEditor, static_cast<int>(style.horizontalAlign),
                                   mixed(SnowCanvasTextStyleMixedHorizontalAlign));
        }
        return;
    }
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.text.property_group_refresh");
    const SnowCanvasTextStyle& style = m_textStyle.textStyle();
    const auto mixed = [this](quint32 property) {
        return m_showingSelectedTextStyle && (m_textStyleMixed & property) != 0;
    };

    updateColorEditor(m_textColorEditor, style.color, mixed(SnowCanvasTextStyleMixedColor));

    updateFontEditor(
        m_textFontEditor, style.fontSize, style.fontFamily, mixed(SnowCanvasTextStyleMixedFontSize),
        mixed(SnowCanvasTextStyleMixedFontFamily), TextFontSizeRefresh | TextFontFamilyRefresh,
        TextFontSizeRefresh, TextFontFamilyRefresh);

    updateWidthColorEditor(m_textStrokeEditor, style.strokeWidth, style.stroke,
                           mixed(SnowCanvasTextStyleMixedStrokeWidth),
                           mixed(SnowCanvasTextStyleMixedStroke));

    updateFillEditor(m_textFillEditor, style.fill, style.fillStyle,
                     mixed(SnowCanvasTextStyleMixedFill), mixed(SnowCanvasTextStyleMixedFillStyle));

    const bool uniformRadius =
        qFuzzyCompare(style.cornerRadii.topLeft + 1.0, style.cornerRadii.topRight + 1.0) &&
        qFuzzyCompare(style.cornerRadii.topLeft + 1.0, style.cornerRadii.bottomRight + 1.0) &&
        qFuzzyCompare(style.cornerRadii.topLeft + 1.0, style.cornerRadii.bottomLeft + 1.0);
    if (m_textCornerRadiusEditor != nullptr) {
        m_textCornerRadiusEditor->setCornerRadius(qRound(style.cornerRadii.topLeft));
        m_textCornerRadiusEditor->setMixed(mixed(SnowCanvasTextStyleMixedCornerRadii) ||
                                           !uniformRadius);
    }

    updateIconOptionEditor(m_textAlignmentEditor, static_cast<int>(style.horizontalAlign),
                           mixed(SnowCanvasTextStyleMixedHorizontalAlign));
}

void ScreenshotToolPaletteStyleControls::notifyTextStyleChanged() const {
    if (m_callbacks.textStyleChanged) {
        m_callbacks.textStyleChanged(m_textStyle.textStyle());
    }
}

void ScreenshotToolPaletteStyleControls::setSerialNumberColor(const QColor& color) {
    const bool wasMixed = (m_serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedColor) != 0;
    if (!color.isValid() || (m_serialNumberStyle.color == color && !wasMixed)) {
        return;
    }
    m_serialNumberStyle.color = color;
    m_creationSerialNumberStyle.color = color;
    m_serialNumberStyleMixed &= ~SnowCanvasSerialNumberStyleMixedColor;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFillColor(const QColor& color) {
    const bool wasMixed = (m_serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFill) != 0;
    if (!color.isValid() || (m_serialNumberStyle.fill == color && !wasMixed)) {
        return;
    }
    m_serialNumberStyle.fill = color;
    m_creationSerialNumberStyle.fill = color;
    m_serialNumberStyleMixed &= ~SnowCanvasSerialNumberStyleMixedFill;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFillStyle(SnowCanvasFillStyle fillStyle) {
    const bool wasMixed =
        (m_serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFillStyle) != 0;
    if (m_serialNumberStyle.fillStyle == fillStyle && !wasMixed) {
        return;
    }
    m_serialNumberStyle.fillStyle = fillStyle;
    m_creationSerialNumberStyle.fillStyle = fillStyle;
    m_serialNumberStyleMixed &= ~SnowCanvasSerialNumberStyleMixedFillStyle;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setSerialNumber(qint64 number) {
    number = std::max<qint64>(0, number);
    const bool wasMixed = (m_serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedNumber) != 0;
    if (m_serialNumberStyle.number == number && !wasMixed) {
        return;
    }
    m_serialNumberStyle.number = number;
    m_creationSerialNumberStyle.number = number;
    m_serialNumberStyleMixed &= ~SnowCanvasSerialNumberStyleMixedNumber;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

void ScreenshotToolPaletteStyleControls::setSerialNumberFontSize(double fontSize) {
    fontSize = std::clamp(fontSize, 6.0, 512.0);
    const bool wasMixed =
        (m_serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFontSize) != 0;
    if (qFuzzyCompare(m_serialNumberStyle.fontSize + 1.0, fontSize + 1.0) && !wasMixed) {
        return;
    }
    m_serialNumberStyle.fontSize = fontSize;
    m_creationSerialNumberStyle.fontSize = fontSize;
    m_serialNumberStyleMixed &= ~SnowCanvasSerialNumberStyleMixedFontSize;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

void ScreenshotToolPaletteStyleControls::cycleSerialNumberFontSize() {
    const QVector<double>& values = m_textStyle.fontSizeValues();
    for (double value : values) {
        if (value > m_serialNumberStyle.fontSize) {
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
    const bool wasMixed =
        (m_serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFontFamily) != 0;
    if (m_serialNumberStyle.fontFamily == normalized && !wasMixed) {
        return;
    }
    m_serialNumberStyle.fontFamily = normalized;
    m_creationSerialNumberStyle.fontFamily = normalized;
    m_serialNumberStyleMixed &= ~SnowCanvasSerialNumberStyleMixedFontFamily;
    updateSerialNumberStyleControls();
    notifySerialNumberStyleChanged();
}

void ScreenshotToolPaletteStyleControls::updateSerialNumberStyleControls(quint32 groups) {
#if defined(SNOW_SHOT_TEST_HOOKS)
    m_propertyGroupRefreshCount += propertyGroupCount(groups, AllSerialNumberRefreshes);
#endif
    if (groups != 0xffffffffu) {
        const auto mixed = [this](quint32 property) {
            return (m_serialNumberStyleMixed & property) != 0;
        };
        if ((groups & SerialNumberColorRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.color_refresh");
            updateColorEditor(m_serialNumberColorEditor, m_serialNumberStyle.color,
                              mixed(SnowCanvasSerialNumberStyleMixedColor));
        }
        if ((groups & SerialNumberFillRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.fill_refresh");
            updateFillEditor(m_serialNumberFillEditor, m_serialNumberStyle.fill,
                             m_serialNumberStyle.fillStyle,
                             mixed(SnowCanvasSerialNumberStyleMixedFill),
                             mixed(SnowCanvasSerialNumberStyleMixedFillStyle));
        }
        if ((groups & SerialNumberValueRefresh) != 0 && m_serialNumberEditor != nullptr) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.value_refresh");
            m_serialNumberEditor->setCornerRadius(static_cast<int>(std::clamp<qint64>(
                m_serialNumberStyle.number, 0, std::numeric_limits<int>::max())));
            m_serialNumberEditor->setMixed(mixed(SnowCanvasSerialNumberStyleMixedNumber));
        }
        if ((groups & SerialNumberFontSizeRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.font_size_refresh");
        }
        if ((groups & SerialNumberFontFamilyRefresh) != 0) {
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.font_family_refresh");
        }
        updateFontEditor(m_serialNumberFontEditor, m_serialNumberStyle.fontSize,
                         m_serialNumberStyle.fontFamily,
                         mixed(SnowCanvasSerialNumberStyleMixedFontSize),
                         mixed(SnowCanvasSerialNumberStyleMixedFontFamily), groups,
                         SerialNumberFontSizeRefresh, SerialNumberFontFamilyRefresh);
        return;
    }
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.serial_number.property_group_refresh");
    const auto mixed = [this](quint32 property) {
        return (m_serialNumberStyleMixed & property) != 0;
    };

    updateColorEditor(m_serialNumberColorEditor, m_serialNumberStyle.color,
                      mixed(SnowCanvasSerialNumberStyleMixedColor));

    updateFillEditor(m_serialNumberFillEditor, m_serialNumberStyle.fill,
                     m_serialNumberStyle.fillStyle, mixed(SnowCanvasSerialNumberStyleMixedFill),
                     mixed(SnowCanvasSerialNumberStyleMixedFillStyle));

    if (m_serialNumberEditor != nullptr) {
        m_serialNumberEditor->setCornerRadius(static_cast<int>(
            std::clamp<qint64>(m_serialNumberStyle.number, 0, std::numeric_limits<int>::max())));
        m_serialNumberEditor->setMixed(mixed(SnowCanvasSerialNumberStyleMixedNumber));
    }
    updateFontEditor(m_serialNumberFontEditor, m_serialNumberStyle.fontSize,
                     m_serialNumberStyle.fontFamily,
                     mixed(SnowCanvasSerialNumberStyleMixedFontSize),
                     mixed(SnowCanvasSerialNumberStyleMixedFontFamily),
                     SerialNumberFontSizeRefresh | SerialNumberFontFamilyRefresh,
                     SerialNumberFontSizeRefresh, SerialNumberFontFamilyRefresh);
}

void ScreenshotToolPaletteStyleControls::notifySerialNumberStyleChanged() const {
    if (m_callbacks.serialNumberStyleChanged) {
        m_callbacks.serialNumberStyleChanged(m_serialNumberStyle);
    }
}

bool ScreenshotToolPaletteStyleControls::hasMixedProperty(quint32 property) const {
    return m_showingSelectedStyle && (m_selectedStyleMixed & property) != 0;
}

void ScreenshotToolPaletteStyleControls::clearMixedProperties(quint32 properties) {
    if (m_showingSelectedStyle) {
        m_selectedStyleMixed &= ~properties;
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
