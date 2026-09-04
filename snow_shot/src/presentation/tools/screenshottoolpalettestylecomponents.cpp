#include "screenshottoolpalettestylecomponents.h"

#include "screenshottoolbarperfinstrumentation.h"
#include "screenshottoolpalettestylepresets.h"

#include "widgets/color_picker.h"
#include "widgets/control_scale.h"
#include "widgets/popover.h"
#include "widgets/select.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QObject>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <iterator>

namespace snow_shot::presentation {
namespace {

constexpr int kColorPickerOptionSpacing = 4;
constexpr int kEditorItemSpacing = 4;

void setButtonActive(adqt::widgets::AdButton* button, bool active) {
    if (button == nullptr) {
        return;
    }

    button->setButtonStyle(active ? adqt::widgets::AdButton::ButtonStyle::Tonal
                                  : adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(active ? adqt::widgets::AdButton::AccentRole::Primary
                                 : adqt::widgets::AdButton::AccentRole::Neutral);
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

void activateLayoutTree(QLayout* layout);

void activateWidgetLayoutTree(QWidget* widget) {
    if (widget != nullptr && widget->layout() != nullptr) {
        activateLayoutTree(widget->layout());
    }
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

void configureColorPickerMetrics(adqt::widgets::AdColorPicker* picker,
                                 const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, picker)) {
        return;
    }

    const int buttonSize = qMax(1, qRound(metrics.buttonSize * metrics.physicalScale));
    picker->setFixedSize(buttonSize, buttonSize);
    stampScreenshotToolbarReferenceWidth(picker, metrics.buttonSize);
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

void resetPopupButtonControlScale(adqt::widgets::AdButton* button) {
    if (button == nullptr) {
        return;
    }

    const adqt::widgets::AdControlScaleContext popupContext =
        adqt::widgets::AdControlScaleContext::fromDprs(1.0, 1.0);
    button->prepareControlScale(popupContext);
    button->commitControlScale(popupContext);
}

void observePopupLifecycle(QObject* popup, const ScreenshotToolPaletteEditorServices& services) {
    if (popup == nullptr) {
        return;
    }
    if (auto* picker = qobject_cast<adqt::widgets::AdColorPicker*>(popup)) {
        QObject::connect(picker, &adqt::widgets::AdColorPicker::popupOpening, picker,
                         [services, picker]() {
                             if (services.popupInteractionBegan) {
                                 services.popupInteractionBegan(picker);
                             }
                         });
        QObject::connect(picker, &adqt::widgets::AdColorPicker::popupVisibleChanged, picker,
                         [services, picker](bool visible) {
                             if (!visible && services.popupInteractionEnded) {
                                 services.popupInteractionEnded(picker);
                             }
                         });
        return;
    }
    if (auto* select = qobject_cast<adqt::widgets::AdSelect*>(popup)) {
        QObject::connect(select, &adqt::widgets::AdSelect::popupOpening, select,
                         [services, select]() {
                             if (services.popupInteractionBegan) {
                                 services.popupInteractionBegan(select);
                             }
                         });
        QObject::connect(select, &adqt::widgets::AdSelect::popupVisibleChanged, select,
                         [services, select](bool visible) {
                             if (!visible && services.popupInteractionEnded) {
                                 services.popupInteractionEnded(select);
                             }
                         });
    }
}

struct ColorPickerPopupLayout {
    QWidget* widget = nullptr;
    QBoxLayout* layout = nullptr;
};

ColorPickerPopupLayout
createColorPickerPopupContent(adqt::widgets::AdColorPicker* picker, const QString& objectName,
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

ColorPickerPopupLayout
addColorPickerPopupRow(ColorPickerPopupLayout& content, const QString& objectName,
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

void dispatchColorChange(bool* handlingChange, const std::function<void(const QColor&)>& callback,
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
}

void connectColorPickerChanges(adqt::widgets::AdColorPicker* picker, QObject* receiver,
                               bool* handlingChange,
                               const std::function<void(const QColor&)>& commitColor,
                               const std::function<void(const QColor&)>& previewColor = {},
                               const std::function<void(const QColor&)>& valueChanged = {}) {
    if (picker == nullptr || receiver == nullptr) {
        return;
    }
    QObject::connect(picker, &adqt::widgets::AdColorPicker::valueChanged, receiver,
                     [handlingChange, valueChanged, commitColor,
                      previewColor](const adqt::widgets::AdColorValue& value) {
                         if (valueChanged && value.isSolid() && value.solidColor.isValid()) {
                             valueChanged(value.solidColor);
                         }
                         dispatchColorChange(handlingChange,
                                             previewColor ? previewColor : commitColor, value);
                     });
    if (previewColor) {
        QObject::connect(picker, &adqt::widgets::AdColorPicker::editingFinished, receiver,
                         [handlingChange, commitColor](const adqt::widgets::AdColorValue& value) {
                             dispatchColorChange(handlingChange, commitColor, value);
                         });
    }
}

adqt::widgets::AdColorPicker*
createColorPickerShell(QWidget* parent, const QString& accessibleName, const QColor& initialColor,
                       bool alphaEnabled, bool observePopup,
                       const ScreenshotToolPaletteEditorServices& services) {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* picker = new adqt::widgets::AdColorPicker(parent);
    if (observePopup) {
        observePopupLifecycle(picker, services);
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
    QObject::connect(sampler, &QAbstractButton::clicked, picker, [services, picker]() {
        picker->setPopupVisible(false);
        if (services.canvasColorSamplingRequested) {
            services.canvasColorSamplingRequested(picker);
        }
    });
    return picker;
}

void resetPickerPopupContent(adqt::widgets::AdColorPicker* picker,
                             const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (picker == nullptr) {
        return;
    }
    if (QWidget* content = picker->popupContent()) {
        for (adqt::widgets::AdButton* button : content->findChildren<adqt::widgets::AdButton*>()) {
            resetPopupButtonControlScale(button);
        }
    }
    if (screenshotToolPaletteMetricsApplyTo(metrics, picker)) {
        activateWidgetLayoutTree(picker);
    }
}

} // namespace

QBoxLayout* ScreenshotToolPaletteStyleEditorComponent::createRoot(QBoxLayout* layout,
                                                                  QWidget* parent) {
    if (layout == nullptr || parent == nullptr) {
        return nullptr;
    }
    m_rootWidget = new QWidget(parent);
    m_rootWidget->setProperty("screenshotStyleEditorRoot", true);
    m_rootWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* rootLayout = new QHBoxLayout(m_rootWidget);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(kEditorItemSpacing);
    layout->addWidget(m_rootWidget);
    return rootLayout;
}

void ScreenshotToolPaletteStyleEditorComponent::finalizeRoot() {
    if (m_rootWidget == nullptr || m_rootWidget->layout() == nullptr) {
        return;
    }
    auto* layout = static_cast<QBoxLayout*>(m_rootWidget->layout());
    int referenceWidth = 0;
    int visibleItems = 0;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item == nullptr || item->widget() == nullptr) {
            continue;
        }
        const int itemWidth = screenshotToolbarReferenceWidth(item->widget());
        referenceWidth += itemWidth > 0 ? itemWidth : item->widget()->sizeHint().width();
        ++visibleItems;
    }
    referenceWidth += std::max(0, visibleItems - 1) * kEditorItemSpacing;
    stampScreenshotToolbarReferenceWidth(m_rootWidget, referenceWidth);
}

void ScreenshotToolPaletteStyleEditorComponent::refreshRootMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, m_rootWidget) ||
        m_rootWidget->layout() == nullptr) {
        return;
    }
    m_rootWidget->layout()->setSpacing(qMax(0, qRound(kEditorItemSpacing * metrics.physicalScale)));
    m_rootWidget->layout()->invalidate();
}

void ScreenshotToolPaletteStyleEditorComponent::releaseRoot() {
    m_rootWidget = nullptr;
}

void ScreenshotToolPaletteColorEditor::build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                             const ScreenshotToolPaletteColorEditorConfig& config,
                                             const QColor& initialColor,
                                             const std::function<void(const QColor&)>& commitColor,
                                             const std::function<void(const QColor&)>& previewColor,
                                             const ScreenshotToolPaletteEditorServices& services,
                                             const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_color_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_commitColor = std::make_shared<std::function<void(const QColor&)>>(commitColor);
    m_previewColor = std::make_shared<std::function<void(const QColor&)>>(previewColor);

    m_presetValues = config.presetValues;
    m_picker = createColorPickerShell(parent, config.accessibleName, initialColor,
                                      config.alphaEnabled, config.observePopup, services);
    if (!config.pickerObjectName.isEmpty()) {
        m_picker->setObjectName(config.pickerObjectName);
    }
    m_trigger = createScreenshotToolPaletteColorButton(
        m_picker, config.accessibleName.toUtf8().constData(), initialColor, true, true, metrics);
    if (!config.triggerObjectName.isEmpty()) {
        m_trigger->setObjectName(config.triggerObjectName);
    }
    configureStylePopupTrigger(m_trigger, config.accessibleName);
    m_picker->setTriggerContent(m_trigger);
    configureColorPickerMetrics(m_picker, metrics);
    layout->addWidget(m_picker);

    QObject::connect(m_picker, &adqt::widgets::AdColorPicker::valueChanged, receiver,
                     [handlingChange = &m_handlingChange, commit = m_commitColor,
                      preview = m_previewColor,
                      trigger = m_trigger](const adqt::widgets::AdColorValue& value) {
                         const auto updateTrigger = [trigger](const QColor& color) {
                             if (trigger != nullptr) {
                                 trigger->setSwatchColor(color);
                             }
                         };
                         if (value.isSolid() && value.solidColor.isValid()) {
                             updateTrigger(value.solidColor);
                         }
                         if (preview != nullptr && *preview) {
                             dispatchColorChange(handlingChange, *preview, value);
                         } else if (commit != nullptr) {
                             dispatchColorChange(handlingChange, *commit, value);
                         }
                     });
    QObject::connect(m_picker, &adqt::widgets::AdColorPicker::editingFinished, receiver,
                     [handlingChange = &m_handlingChange, commit = m_commitColor,
                      preview = m_previewColor](const adqt::widgets::AdColorValue& value) {
                         if (preview != nullptr && *preview && commit != nullptr) {
                             dispatchColorChange(handlingChange, *commit, value);
                         }
                     });

    for (const QColor& color : m_presetValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.presetTooltip ? config.presetTooltip(color)
                                 : ScreenshotToolPaletteTranslationText(color.name());
        auto* button =
            createScreenshotToolPaletteColorButton(parent, nullptr, color, false, true, metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, color == initialColor);
        m_presets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_commitColor, color]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(color);
                             }
                         });
    }
    finalizeRoot();
}

void ScreenshotToolPaletteColorEditor::update(const QColor& color, bool mixed) {
    if (m_picker != nullptr) {
        const QSignalBlocker blocker(m_picker);
        m_picker->setValue(adqt::widgets::AdColorValue::solid(color));
    }
    if (m_trigger != nullptr) {
        m_trigger->setSwatchColor(color);
    }
    for (int index = 0; index < m_presets.size() && index < m_presetValues.size(); ++index) {
        setButtonActive(m_presets.at(index), !mixed && m_presetValues.at(index) == color);
    }
}

void ScreenshotToolPaletteColorEditor::rebind(
    const ScreenshotToolPaletteColorEditorConfig& config,
    const std::function<void(const QColor&)>& commitColor,
    const std::function<void(const QColor&)>& previewColor) {
    if (m_commitColor != nullptr) {
        *m_commitColor = commitColor;
    }
    if (m_previewColor != nullptr) {
        *m_previewColor = previewColor;
    }
    if (m_picker != nullptr) {
        configureStylePopupTrigger(m_picker, config.accessibleName);
        m_picker->setObjectName(config.pickerObjectName);
    }
    if (m_trigger != nullptr) {
        configureStylePopupTrigger(m_trigger, config.accessibleName);
        m_trigger->setObjectName(config.triggerObjectName);
    }
    for (int index = 0; index < m_presets.size() && index < m_presetValues.size(); ++index) {
        const QColor& color = m_presetValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_presets.at(index), config.presetTooltip
                                     ? config.presetTooltip(color)
                                     : ScreenshotToolPaletteTranslationText(color.name()));
    }
}

void ScreenshotToolPaletteColorEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    configureColorPickerMetrics(m_picker, metrics);
    if (screenshotToolPaletteMetricsApplyTo(metrics, m_trigger)) {
        configureScreenshotToolPaletteStyleButton(m_trigger, nullptr, metrics);
        m_trigger->setPhysicalScale(metrics.physicalScale);
    }
    for (adqt::widgets::AdButton* button : m_presets) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        if (auto* swatch = dynamic_cast<ColorSwatchButton*>(button)) {
            swatch->setPhysicalScale(metrics.physicalScale);
        }
    }
}

void ScreenshotToolPaletteColorEditor::resetPopupMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    resetPickerPopupContent(m_picker, metrics);
}

void ScreenshotToolPaletteColorEditor::release() {
    m_picker = nullptr;
    m_trigger = nullptr;
    m_presets.clear();
    m_presetValues.clear();
    m_commitColor.reset();
    m_previewColor.reset();
    m_handlingChange = false;
    releaseRoot();
}

void ScreenshotToolPaletteStrokeEditor::build(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteStrokeEditorConfig& config, const QColor& initialColor,
    SnowCanvasStrokeStyle initialStyle, const std::function<void(const QColor&)>& setColor,
    const std::function<void(SnowCanvasStrokeStyle)>& setStyle,
    const ScreenshotToolPaletteEditorServices& services,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_stroke_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_setColor = std::make_shared<std::function<void(const QColor&)>>(setColor);
    m_setStyle = std::make_shared<std::function<void(SnowCanvasStrokeStyle)>>(setStyle);

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    m_colorValues = config.colorValues;
    m_styleValues = {
        SnowCanvasStrokeStyle::Solid,
        SnowCanvasStrokeStyle::Dashed,
        SnowCanvasStrokeStyle::Dotted,
    };
    m_picker =
        createColorPickerShell(parent, config.accessibleName, initialColor, false, false, services);
    m_trigger = createScreenshotToolPaletteStrokeStyleTrigger(
        m_picker, config.accessibleName.toUtf8().constData(), initialColor, initialStyle, metrics);
    configureStylePopupTrigger(m_trigger, config.accessibleName);
    m_picker->setTriggerContent(m_trigger);

    ColorPickerPopupLayout popupContent =
        createColorPickerPopupContent(m_picker, config.popupObjectName, popupMetrics);
    ColorPickerPopupLayout styleRow =
        addColorPickerPopupRow(popupContent, config.styleRowObjectName, popupMetrics);
    for (SnowCanvasStrokeStyle style : m_styleValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.styleTooltip
                ? config.styleTooltip(style)
                : ScreenshotToolPaletteTranslationText(QString::number(static_cast<int>(style)));
        auto* button = createScreenshotToolPaletteStrokeStyleButton(styleRow.widget, nullptr, style,
                                                                    popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, style == initialStyle);
        m_styleButtons.push_back(button);
        styleRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setStyle, style]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(style);
                             }
                         });
    }
    styleRow.layout->addStretch(1);
    m_picker->setPopupContent(popupContent.widget);
    configureColorPickerMetrics(m_picker, metrics);
    layout->addWidget(m_picker);

    connectColorPickerChanges(m_picker, receiver, &m_handlingChange,
                              [callback = m_setColor](const QColor& color) {
                                  if (callback != nullptr && *callback) {
                                      (*callback)(color);
                                  }
                              });

    for (const QColor& color : m_colorValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.colorTooltip ? config.colorTooltip(color)
                                : ScreenshotToolPaletteTranslationText(color.name());
        auto* button =
            createScreenshotToolPaletteColorButton(parent, nullptr, color, false, true, metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, color == initialColor);
        m_colorPresets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setColor, color]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(color);
                             }
                         });
    }
    finalizeRoot();
}

void ScreenshotToolPaletteStrokeEditor::update(const QColor& color, SnowCanvasStrokeStyle style,
                                               bool colorMixed, bool styleMixed) {
    if (m_picker != nullptr) {
        const QSignalBlocker blocker(m_picker);
        m_picker->setValue(adqt::widgets::AdColorValue::solid(color));
    }
    if (m_trigger != nullptr) {
        m_trigger->setStrokeColor(color);
        m_trigger->setStrokeStyle(style);
        m_trigger->setMixed(colorMixed || styleMixed);
    }
    for (int index = 0; index < m_colorPresets.size() && index < m_colorValues.size(); ++index) {
        setButtonActive(m_colorPresets.at(index), !colorMixed && m_colorValues.at(index) == color);
    }
    for (int index = 0; index < m_styleButtons.size() && index < m_styleValues.size(); ++index) {
        setButtonActive(m_styleButtons.at(index), !styleMixed && m_styleValues.at(index) == style);
    }
}

void ScreenshotToolPaletteStrokeEditor::rebind(
    const ScreenshotToolPaletteStrokeEditorConfig& config,
    const std::function<void(const QColor&)>& setColor,
    const std::function<void(SnowCanvasStrokeStyle)>& setStyle) {
    if (m_setColor != nullptr) {
        *m_setColor = setColor;
    }
    if (m_setStyle != nullptr) {
        *m_setStyle = setStyle;
    }
    if (m_picker != nullptr) {
        configureStylePopupTrigger(m_picker, config.accessibleName);
        if (QWidget* popup = m_picker->popupContent()) {
            popup->setObjectName(config.popupObjectName);
            if (popup->layout() != nullptr && popup->layout()->count() > 0 &&
                popup->layout()->itemAt(0)->widget() != nullptr) {
                popup->layout()->itemAt(0)->widget()->setObjectName(config.styleRowObjectName);
            }
        }
    }
    if (m_trigger != nullptr) {
        configureStylePopupTrigger(m_trigger, config.accessibleName);
    }
    for (int index = 0; index < m_colorPresets.size() && index < m_colorValues.size(); ++index) {
        const QColor& color = m_colorValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_colorPresets.at(index), config.colorTooltip
                                          ? config.colorTooltip(color)
                                          : ScreenshotToolPaletteTranslationText(color.name()));
    }
    for (int index = 0; index < m_styleButtons.size() && index < m_styleValues.size(); ++index) {
        const SnowCanvasStrokeStyle style = m_styleValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_styleButtons.at(index),
            config.styleTooltip
                ? config.styleTooltip(style)
                : ScreenshotToolPaletteTranslationText(QString::number(static_cast<int>(style))));
    }
}

void ScreenshotToolPaletteStrokeEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    configureColorPickerMetrics(m_picker, metrics);
    configureScreenshotToolPaletteStrokeStyleTrigger(m_trigger, metrics);
    for (adqt::widgets::AdButton* button : m_colorPresets) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        if (auto* swatch = dynamic_cast<ColorSwatchButton*>(button)) {
            swatch->setPhysicalScale(metrics.physicalScale);
        }
    }
}

void ScreenshotToolPaletteStrokeEditor::resetPopupMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    resetPickerPopupContent(m_picker, metrics);
}

void ScreenshotToolPaletteStrokeEditor::release() {
    m_picker = nullptr;
    m_trigger = nullptr;
    m_colorPresets.clear();
    m_colorValues.clear();
    m_styleButtons.clear();
    m_styleValues.clear();
    m_setColor.reset();
    m_setStyle.reset();
    m_handlingChange = false;
    releaseRoot();
}

void ScreenshotToolPaletteFillEditor::build(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteFillEditorConfig& config, const QColor& initialColor,
    SnowCanvasFillStyle initialStyle, const std::function<void(const QColor&)>& setColor,
    const std::function<void(SnowCanvasFillStyle)>& setStyle,
    const ScreenshotToolPaletteEditorServices& services,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_fill_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_setColor = std::make_shared<std::function<void(const QColor&)>>(setColor);
    m_setStyle = std::make_shared<std::function<void(SnowCanvasFillStyle)>>(setStyle);

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    m_colorValues = config.colorValues;
    m_styleValues = {
        SnowCanvasFillStyle::Solid,
        SnowCanvasFillStyle::CrossLine,
        SnowCanvasFillStyle::Line,
    };
    m_picker = createColorPickerShell(parent, config.accessibleName, initialColor, true,
                                      config.observePopup, services);
    m_trigger = createScreenshotToolPaletteFillStyleTrigger(
        m_picker, config.accessibleName.toUtf8().constData(), initialColor, initialStyle, metrics);
    configureStylePopupTrigger(m_trigger, config.accessibleName);
    m_picker->setTriggerContent(m_trigger);

    ColorPickerPopupLayout popupContent =
        createColorPickerPopupContent(m_picker, config.popupObjectName, popupMetrics);
    ColorPickerPopupLayout presetRow =
        addColorPickerPopupRow(popupContent, config.presetRowObjectName, popupMetrics);
    for (const QColor& color : m_colorValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.colorTooltip ? config.colorTooltip(color)
                                : ScreenshotToolPaletteTranslationText(color.name());
        auto* button = createScreenshotToolPaletteColorButton(presetRow.widget, nullptr, color,
                                                              false, true, popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, color == initialColor);
        m_colorPresets.push_back(button);
        presetRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setColor, color]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(color);
                             }
                         });
    }
    presetRow.layout->addStretch(1);
    m_picker->setPopupContent(popupContent.widget);
    configureColorPickerMetrics(m_picker, metrics);
    layout->addWidget(m_picker);

    connectColorPickerChanges(m_picker, receiver, &m_handlingChange,
                              [callback = m_setColor](const QColor& color) {
                                  if (callback != nullptr && *callback) {
                                      (*callback)(color);
                                  }
                              });

    for (SnowCanvasFillStyle style : m_styleValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.styleTooltip
                ? config.styleTooltip(style)
                : ScreenshotToolPaletteTranslationText(QString::number(static_cast<int>(style)));
        auto* button = createScreenshotToolPaletteFillStyleButton(parent, nullptr, QColor(), style,
                                                                  false, metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        setButtonActive(button, style == initialStyle);
        m_styleButtons.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setStyle, style]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(style);
                             }
                         });
    }
    finalizeRoot();
}

void ScreenshotToolPaletteFillEditor::update(const QColor& color, SnowCanvasFillStyle style,
                                             bool colorMixed, bool styleMixed) {
    if (m_picker != nullptr) {
        const QSignalBlocker blocker(m_picker);
        m_picker->setValue(adqt::widgets::AdColorValue::solid(color));
    }
    if (m_trigger != nullptr) {
        m_trigger->setFillColor(color);
        m_trigger->setFillStyle(style);
        m_trigger->setMixed(colorMixed || styleMixed);
    }
    for (int index = 0; index < m_colorPresets.size() && index < m_colorValues.size(); ++index) {
        setButtonActive(m_colorPresets.at(index), !colorMixed && m_colorValues.at(index) == color);
    }
    for (int index = 0; index < m_styleButtons.size() && index < m_styleValues.size(); ++index) {
        setButtonActive(m_styleButtons.at(index), !styleMixed && m_styleValues.at(index) == style);
    }
}

void ScreenshotToolPaletteFillEditor::rebind(
    const ScreenshotToolPaletteFillEditorConfig& config,
    const std::function<void(const QColor&)>& setColor,
    const std::function<void(SnowCanvasFillStyle)>& setStyle) {
    if (m_setColor != nullptr) {
        *m_setColor = setColor;
    }
    if (m_setStyle != nullptr) {
        *m_setStyle = setStyle;
    }
    if (m_picker != nullptr) {
        configureStylePopupTrigger(m_picker, config.accessibleName);
        if (QWidget* popup = m_picker->popupContent()) {
            popup->setObjectName(config.popupObjectName);
            if (popup->layout() != nullptr && popup->layout()->count() > 0 &&
                popup->layout()->itemAt(0)->widget() != nullptr) {
                popup->layout()->itemAt(0)->widget()->setObjectName(config.presetRowObjectName);
            }
        }
    }
    if (m_trigger != nullptr) {
        configureStylePopupTrigger(m_trigger, config.accessibleName);
    }
    for (int index = 0; index < m_colorPresets.size() && index < m_colorValues.size(); ++index) {
        const QColor& color = m_colorValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_colorPresets.at(index), config.colorTooltip
                                          ? config.colorTooltip(color)
                                          : ScreenshotToolPaletteTranslationText(color.name()));
    }
    for (int index = 0; index < m_styleButtons.size() && index < m_styleValues.size(); ++index) {
        const SnowCanvasFillStyle style = m_styleValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_styleButtons.at(index),
            config.styleTooltip
                ? config.styleTooltip(style)
                : ScreenshotToolPaletteTranslationText(QString::number(static_cast<int>(style))));
    }
}

void ScreenshotToolPaletteFillEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    configureColorPickerMetrics(m_picker, metrics);
    configureScreenshotToolPaletteFillStyleTrigger(m_trigger, metrics);
    for (FillStylePreviewButton* button : m_styleButtons) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        button->setPhysicalScale(metrics.physicalScale);
    }
}

void ScreenshotToolPaletteFillEditor::resetPopupMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    resetPickerPopupContent(m_picker, metrics);
}

void ScreenshotToolPaletteFillEditor::release() {
    m_picker = nullptr;
    m_trigger = nullptr;
    m_colorPresets.clear();
    m_colorValues.clear();
    m_styleButtons.clear();
    m_styleValues.clear();
    m_setColor.reset();
    m_setStyle.reset();
    m_handlingChange = false;
    releaseRoot();
}

void ScreenshotToolPaletteWidthColorEditor::build(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteWidthColorEditorConfig& config, double initialWidth,
    const QColor& initialColor, const std::function<void(double)>& setWidth,
    const std::function<void(const QColor&)>& setColor,
    const ScreenshotToolPaletteEditorServices& services,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_width_color_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_setWidth = std::make_shared<std::function<void(double)>>(setWidth);
    m_setColor = std::make_shared<std::function<void(const QColor&)>>(setColor);

    m_widthValues = config.widthValues;
    m_colorValues = config.colorValues;
    m_picker = createColorPickerShell(parent, config.accessibleName, initialColor, false,
                                      config.observePopup, services);

    m_trigger = createScreenshotToolPaletteStrokeWidthButton(
        m_picker, config.triggerTooltip.toUtf8().constData(), initialWidth, true, metrics);
    configureStylePopupTrigger(m_trigger, config.accessibleName);
    m_picker->setTriggerContent(m_trigger);

    const ScreenshotToolPaletteButtonMetrics popupMetrics = popupButtonMetrics(metrics);
    ColorPickerPopupLayout popupContent =
        createColorPickerPopupContent(m_picker, config.popupObjectName, popupMetrics);
    ColorPickerPopupLayout widthRow =
        addColorPickerPopupRow(popupContent, config.widthRowObjectName, popupMetrics);
    for (double width : m_widthValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.widthTooltip ? config.widthTooltip(width)
                                : ScreenshotToolPaletteTranslationText(QString::number(width));
        auto* button = createScreenshotToolPaletteStrokeWidthButton(widthRow.widget, nullptr, width,
                                                                    false, popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        m_widthButtons.push_back(button);
        widthRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setWidth, width]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(width);
                             }
                         });
    }
    widthRow.layout->addStretch(1);

    ColorPickerPopupLayout colorRow =
        addColorPickerPopupRow(popupContent, config.colorRowObjectName, popupMetrics);
    for (const QColor& color : m_colorValues) {
        const ScreenshotToolPaletteTranslationText tooltip =
            config.colorTooltip ? config.colorTooltip(color)
                                : ScreenshotToolPaletteTranslationText(color.name());
        auto* button = createScreenshotToolPaletteColorButton(colorRow.widget, nullptr, color,
                                                              false, true, popupMetrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        m_colorButtons.push_back(button);
        colorRow.layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setColor, color]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(color);
                             }
                         });
    }
    colorRow.layout->addStretch(1);
    m_picker->setPopupContent(popupContent.widget);
    configureColorPickerMetrics(m_picker, metrics);
    layout->addWidget(m_picker);

    connectColorPickerChanges(m_picker, receiver, &m_handlingChange,
                              [callback = m_setColor](const QColor& color) {
                                  if (callback != nullptr && *callback) {
                                      (*callback)(color);
                                  }
                              });
    update(initialWidth, initialColor, false, false);
    finalizeRoot();
}

void ScreenshotToolPaletteWidthColorEditor::update(double width, const QColor& color,
                                                   bool widthMixed, bool colorMixed) {
    if (m_picker != nullptr) {
        const QSignalBlocker blocker(m_picker);
        m_picker->setValue(adqt::widgets::AdColorValue::solid(color));
        m_picker->setProperty("mixed", colorMixed);
    }
    if (m_trigger != nullptr) {
        m_trigger->setStrokeWidth(width);
        m_trigger->setMixed(widthMixed || colorMixed);
    }
    for (int index = 0; index < m_widthButtons.size() && index < m_widthValues.size(); ++index) {
        setButtonActive(m_widthButtons.at(index),
                        !widthMixed && qFuzzyCompare(m_widthValues.at(index) + 1.0, width + 1.0));
    }
    for (int index = 0; index < m_colorButtons.size() && index < m_colorValues.size(); ++index) {
        setButtonActive(m_colorButtons.at(index), !colorMixed && m_colorValues.at(index) == color);
    }
}

void ScreenshotToolPaletteWidthColorEditor::rebind(
    const ScreenshotToolPaletteWidthColorEditorConfig& config,
    const std::function<void(double)>& setWidth,
    const std::function<void(const QColor&)>& setColor) {
    if (m_setWidth != nullptr) {
        *m_setWidth = setWidth;
    }
    if (m_setColor != nullptr) {
        *m_setColor = setColor;
    }
    if (m_picker != nullptr) {
        configureStylePopupTrigger(m_picker, config.accessibleName);
        if (QWidget* popup = m_picker->popupContent()) {
            popup->setObjectName(config.popupObjectName);
            if (popup->layout() != nullptr && popup->layout()->count() >= 2) {
                if (popup->layout()->itemAt(0)->widget() != nullptr) {
                    popup->layout()->itemAt(0)->widget()->setObjectName(config.widthRowObjectName);
                }
                if (popup->layout()->itemAt(1)->widget() != nullptr) {
                    popup->layout()->itemAt(1)->widget()->setObjectName(config.colorRowObjectName);
                }
            }
        }
    }
    if (m_trigger != nullptr) {
        configureStylePopupTrigger(m_trigger, config.accessibleName);
    }
    for (int index = 0; index < m_widthButtons.size() && index < m_widthValues.size(); ++index) {
        const double width = m_widthValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_widthButtons.at(index),
            config.widthTooltip ? config.widthTooltip(width)
                                : ScreenshotToolPaletteTranslationText(QString::number(width)));
    }
    for (int index = 0; index < m_colorButtons.size() && index < m_colorValues.size(); ++index) {
        const QColor& color = m_colorValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_colorButtons.at(index), config.colorTooltip
                                          ? config.colorTooltip(color)
                                          : ScreenshotToolPaletteTranslationText(color.name()));
    }
}

void ScreenshotToolPaletteWidthColorEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    configureColorPickerMetrics(m_picker, metrics);
    if (screenshotToolPaletteMetricsApplyTo(metrics, m_trigger)) {
        configureScreenshotToolPaletteStyleButton(m_trigger, nullptr, metrics);
        m_trigger->setPhysicalScale(metrics.physicalScale);
    }
}

void ScreenshotToolPaletteWidthColorEditor::resetPopupMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    resetPickerPopupContent(m_picker, metrics);
}

void ScreenshotToolPaletteWidthColorEditor::release() {
    m_picker = nullptr;
    m_trigger = nullptr;
    m_widthButtons.clear();
    m_widthValues.clear();
    m_colorButtons.clear();
    m_colorValues.clear();
    m_setWidth.reset();
    m_setColor.reset();
    m_handlingChange = false;
    releaseRoot();
}

void ScreenshotToolPaletteNumericPresetEditor::build(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteNumericPresetEditorConfig& config, double initialValue,
    const std::function<void()>& cycleValue, const std::function<void(double)>& setValue,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_numeric_preset_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_cycleValue = std::make_shared<std::function<void()>>(cycleValue);
    m_setValue = std::make_shared<std::function<void(double)>>(setValue);

    m_values = config.values;
    m_strokePreview = config.strokePreview;
    m_summary =
        config.strokePreview
            ? static_cast<adqt::widgets::AdButton*>(createScreenshotToolPaletteStrokeWidthButton(
                  parent, config.summaryTooltip.toUtf8().constData(), initialValue, true, metrics))
            : static_cast<adqt::widgets::AdButton*>(createScreenshotToolPaletteNumericValueButton(
                  parent, config.summaryTooltip.toUtf8().constData(), initialValue, config.suffix,
                  metrics));
    if (!config.summaryObjectName.isEmpty()) {
        m_summary->setObjectName(config.summaryObjectName);
    }
    layout->addWidget(m_summary);
    QObject::connect(m_summary, &adqt::widgets::AdButton::clicked, receiver,
                     [callback = m_cycleValue]() {
                         if (callback != nullptr && *callback) {
                             (*callback)();
                         }
                     });

    for (int index = 0; index < m_values.size(); ++index) {
        const double value = m_values.at(index);
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
        m_presets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setValue, value]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(value);
                             }
                         });
    }
    update(initialValue, false);
    finalizeRoot();
}

void ScreenshotToolPaletteNumericPresetEditor::update(double value, bool mixed) {
    if (auto* strokeSummary = dynamic_cast<StrokeWidthPreviewButton*>(m_summary)) {
        strokeSummary->setStrokeWidth(value);
        strokeSummary->setActiveStrokeWidth(true);
        strokeSummary->setMixed(mixed);
    } else if (auto* numericSummary = dynamic_cast<NumericValuePreviewButton*>(m_summary)) {
        numericSummary->setValue(value);
        numericSummary->setMixed(mixed);
    }
    for (int index = 0; index < m_presets.size() && index < m_values.size(); ++index) {
        const bool active = !mixed && qFuzzyCompare(m_values.at(index) + 1.0, value + 1.0);
        setButtonActive(m_presets.at(index), active);
        if (auto* strokeButton = dynamic_cast<StrokeWidthPreviewButton*>(m_presets.at(index))) {
            strokeButton->setStrokeWidth(m_values.at(index));
            strokeButton->setActiveStrokeWidth(active);
        }
    }
}

void ScreenshotToolPaletteNumericPresetEditor::rebind(
    const ScreenshotToolPaletteNumericPresetEditorConfig& config,
    const std::function<void()>& cycleValue, const std::function<void(double)>& setValue) {
    if (m_cycleValue != nullptr) {
        *m_cycleValue = cycleValue;
    }
    if (m_setValue != nullptr) {
        *m_setValue = setValue;
    }
    if (m_summary != nullptr) {
        configureScreenshotToolPaletteTooltip(
            m_summary, ScreenshotToolPaletteTranslationText(config.summaryTooltip));
        m_summary->setObjectName(config.summaryObjectName);
    }
    for (int index = 0; index < m_presets.size() && index < m_values.size(); ++index) {
        const double value = m_values.at(index);
        configureScreenshotToolPaletteTooltip(
            m_presets.at(index),
            config.presetTooltip ? config.presetTooltip(index, value)
                                 : ScreenshotToolPaletteTranslationText(QString::number(value)));
        m_presets.at(index)->setObjectName(config.presetObjectName ? config.presetObjectName(value)
                                                                   : QString());
    }
}

void ScreenshotToolPaletteNumericPresetEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    if (screenshotToolPaletteMetricsApplyTo(metrics, m_summary)) {
        configureScreenshotToolPaletteStyleButton(m_summary, nullptr, metrics);
        if (auto* stroke = dynamic_cast<StrokeWidthPreviewButton*>(m_summary)) {
            stroke->setPhysicalScale(metrics.physicalScale);
        }
        if (auto* numeric = dynamic_cast<NumericValuePreviewButton*>(m_summary)) {
            numeric->setPhysicalScale(metrics.physicalScale);
        }
    }
    for (adqt::widgets::AdButton* button : m_presets) {
        if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
            continue;
        }
        configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        if (auto* stroke = dynamic_cast<StrokeWidthPreviewButton*>(button)) {
            stroke->setPhysicalScale(metrics.physicalScale);
        }
    }
}

void ScreenshotToolPaletteNumericPresetEditor::release() {
    m_summary = nullptr;
    m_presets.clear();
    m_values.clear();
    m_cycleValue.reset();
    m_setValue.reset();
    m_strokePreview = false;
    releaseRoot();
}

void ScreenshotToolPaletteFontEditor::build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                            const ScreenshotToolPaletteFontEditorConfig& config,
                                            double initialSize, const QString& initialFamily,
                                            const std::function<void()>& cycleSize,
                                            const std::function<void(double)>& setSize,
                                            const std::function<void(const QString&)>& setFamily,
                                            const ScreenshotToolPaletteEditorServices& services,
                                            const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_font_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_cycleSize = std::make_shared<std::function<void()>>(cycleSize);
    m_setSize = std::make_shared<std::function<void(double)>>(setSize);
    m_setFamily = std::make_shared<std::function<void(const QString&)>>(setFamily);

    m_sizeValues = config.sizeValues;
    m_sizeSummary = createScreenshotToolPaletteNumericValueButton(
        parent, config.summaryTooltip.toUtf8().constData(), initialSize, QStringLiteral("px"),
        metrics);
    if (!config.summaryObjectName.isEmpty()) {
        m_sizeSummary->setObjectName(config.summaryObjectName);
    }
    layout->addWidget(m_sizeSummary);
    QObject::connect(m_sizeSummary, &adqt::widgets::AdButton::clicked, receiver,
                     [callback = m_cycleSize]() {
                         if (callback != nullptr && *callback) {
                             (*callback)();
                         }
                     });

    for (int index = 0; index < m_sizeValues.size(); ++index) {
        const double value = m_sizeValues.at(index);
        const ScreenshotToolPaletteTranslationText tooltip =
            config.presetTooltip ? config.presetTooltip(index, value)
                                 : ScreenshotToolPaletteTranslationText(QString::number(value));
        auto* button = createScreenshotToolPaletteStyleActionButton(
            parent, nullptr, style_presets::sizePresetIcon(index), metrics);
        configureScreenshotToolPaletteTooltip(button, tooltip);
        m_sizePresets.push_back(button);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [callback = m_setSize, value]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(value);
                             }
                         });
    }

    ScreenshotToolPaletteSelectEditorConfig selectConfig;
    selectConfig.accessibleName = config.accessibleName;
    selectConfig.placeholder = QStringLiteral("Font family");
    selectConfig.searchEnabled = true;
    const ScreenshotToolPaletteSelectEditor selectEditor =
        createScreenshotToolPaletteSelectEditor(parent, selectConfig, metrics);
    m_familySelect = selectEditor.select;
    if (config.observePopup) {
        observePopupLifecycle(m_familySelect, services);
    }
    configureStylePopupTrigger(m_familySelect, config.accessibleName);
    m_familySelect->setSortComparator(
        [](const adqt::widgets::AdSelect::Option& lhs, const adqt::widgets::AdSelect::Option& rhs) {
            const bool lhsIsDefault = lhs.value.toString().isEmpty();
            const bool rhsIsDefault = rhs.value.toString().isEmpty();
            if (lhsIsDefault != rhsIsDefault) {
                return lhsIsDefault;
            }
            return QString::compare(lhs.label, rhs.label, Qt::CaseInsensitive) < 0;
        });

    auto* fontModel = new QStandardItemModel(m_familySelect);
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
    for (const QString& family : screenshotToolPaletteFontFamilies()) {
        appendFont(family, nullptr, family, true, true);
    }
    m_familySelect->setModel(fontModel);
    m_familySelect->setCurrentData(initialFamily, adqt::widgets::AdSelect::DefaultValueRole);
    layout->addWidget(m_familySelect);
    QObject::connect(m_familySelect, &adqt::widgets::AdSelect::selected, receiver,
                     [callback = m_setFamily](const QVariant& value, const QString&) {
                         if (callback != nullptr && *callback &&
                             value.toString() != QStringLiteral("__mixed__")) {
                             (*callback)(value.toString());
                         }
                     });
    finalizeRoot();
}

void ScreenshotToolPaletteFontEditor::update(double size, const QString& family, bool sizeMixed,
                                             bool familyMixed, quint32 groups, quint32 sizeGroup,
                                             quint32 familyGroup) {
    if ((groups & sizeGroup) != 0) {
        if (m_sizeSummary != nullptr) {
            m_sizeSummary->setValue(size);
            m_sizeSummary->setMixed(sizeMixed);
        }
        for (int index = 0; index < m_sizePresets.size() && index < m_sizeValues.size(); ++index) {
            setButtonActive(m_sizePresets.at(index),
                            !sizeMixed && qFuzzyCompare(m_sizeValues.at(index) + 1.0, size + 1.0));
        }
    }

    if ((groups & familyGroup) == 0 || m_familySelect == nullptr) {
        return;
    }

    const QSignalBlocker blocker(m_familySelect);
    auto* model = qobject_cast<QStandardItemModel*>(m_familySelect->model());
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
                ScreenshotToolPaletteTranslationText("%1 (unavailable)").arg(family);
            auto* item = new QStandardItem(unavailableText.translated());
            setScreenshotToolPaletteItemTranslationSource(item, unavailableText);
            item->setData(family, adqt::widgets::AdSelect::DefaultValueRole);
            item->setEnabled(false);
            model->appendRow(item);
        }
    }
    m_familySelect->setCurrentData(familyMixed ? QVariant(QStringLiteral("__mixed__"))
                                               : QVariant(family),
                                   adqt::widgets::AdSelect::DefaultValueRole);
}

void ScreenshotToolPaletteFontEditor::rebind(const ScreenshotToolPaletteFontEditorConfig& config,
                                             const std::function<void()>& cycleSize,
                                             const std::function<void(double)>& setSize,
                                             const std::function<void(const QString&)>& setFamily) {
    if (m_cycleSize != nullptr) {
        *m_cycleSize = cycleSize;
    }
    if (m_setSize != nullptr) {
        *m_setSize = setSize;
    }
    if (m_setFamily != nullptr) {
        *m_setFamily = setFamily;
    }
    if (m_sizeSummary != nullptr) {
        configureScreenshotToolPaletteTooltip(
            m_sizeSummary, ScreenshotToolPaletteTranslationText(config.summaryTooltip));
        m_sizeSummary->setObjectName(config.summaryObjectName);
    }
    for (int index = 0; index < m_sizePresets.size() && index < m_sizeValues.size(); ++index) {
        const double value = m_sizeValues.at(index);
        configureScreenshotToolPaletteTooltip(
            m_sizePresets.at(index),
            config.presetTooltip ? config.presetTooltip(index, value)
                                 : ScreenshotToolPaletteTranslationText(QString::number(value)));
    }
    configureStylePopupTrigger(m_familySelect, config.accessibleName);
}

void ScreenshotToolPaletteFontEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    const auto applies = [&metrics](const QWidget* widget) {
        return screenshotToolPaletteMetricsApplyTo(metrics, widget);
    };
    if (applies(m_sizeSummary)) {
        configureScreenshotToolPaletteStyleButton(m_sizeSummary, nullptr, metrics);
        m_sizeSummary->setPhysicalScale(metrics.physicalScale);
    }
    for (adqt::widgets::AdButton* button : m_sizePresets) {
        if (applies(button)) {
            configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        }
    }
    ScreenshotToolPaletteSelectEditor selectEditor;
    selectEditor.select = m_familySelect;
    configureScreenshotToolPaletteSelectEditor(selectEditor, metrics);
}

void ScreenshotToolPaletteFontEditor::release() {
    m_sizeSummary = nullptr;
    m_sizePresets.clear();
    m_sizeValues.clear();
    m_familySelect = nullptr;
    m_cycleSize.reset();
    m_setSize.reset();
    m_setFamily.reset();
    releaseRoot();
}

void ScreenshotToolPaletteIconOptionEditor::build(
    QBoxLayout* layout, QWidget* parent, QObject* receiver,
    const ScreenshotToolPaletteIconOptionEditorConfig& config, int initialValue,
    const std::function<void(int)>& setValue, const ScreenshotToolPaletteButtonMetrics& metrics) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("style.add_icon_option_editor");
    if (layout == nullptr || parent == nullptr || receiver == nullptr || config.options.isEmpty()) {
        return;
    }

    layout = createRoot(layout, parent);
    parent = rootWidget();
    receiver = rootWidget();
    if (layout == nullptr) {
        return;
    }

    m_setValue = std::make_shared<std::function<void(int)>>(setValue);

    const auto initialOption =
        std::find_if(config.options.cbegin(), config.options.cend(),
                     [initialValue](const ScreenshotToolPaletteIconOption& option) {
                         return option.value == initialValue;
                     });
    const adqt::icons::IconRef initialIcon = initialOption != config.options.cend()
                                                 ? initialOption->icon
                                                 : config.options.constFirst().icon;
    const QByteArray triggerTooltip = config.triggerTooltip.toUtf8();
    m_trigger = createScreenshotToolPaletteIconValuePreviewTrigger(
        parent, triggerTooltip.constData(), initialIcon, metrics);
    configureStylePopupTrigger(m_trigger, config.accessibleName);
    layout->addWidget(m_trigger);

    m_popover = new adqt::widgets::AdPopover(m_trigger);
    m_popover->setSourceWidget(m_trigger);
    m_popover->setTriggers(adqt::widgets::AdPopover::Trigger::Hover);
    m_popover->setPlacement(adqt::widgets::AdPopover::Placement::Bottom);
    m_popover->setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    m_popover->setArrowVisible(true);
    m_popover->setContentMargins(QMargins(8, 8, 8, 8));
    if (config.minimizeTitleWidth) {
        m_popover->setTitleMinimumWidth(0);
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

    m_options = config.options;
    for (int index = 0; index < m_options.size(); ++index) {
        const ScreenshotToolPaletteIconOption& option = m_options.at(index);
        auto* button = createScreenshotToolPaletteStyleActionButton(content, nullptr, option.icon,
                                                                    popupMetrics);
        configureScreenshotToolPaletteTooltip(button, option.tooltip);
        setButtonActive(button, option.value == initialValue);
        m_buttons.push_back(button);
        if (gridLayout != nullptr) {
            gridLayout->addWidget(button, index / config.gridColumnCount,
                                  index % config.gridColumnCount);
        } else {
            static_cast<QHBoxLayout*>(optionLayout)->addWidget(button);
        }
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [popover = m_popover, callback = m_setValue, value = option.value]() {
                             if (callback != nullptr && *callback) {
                                 (*callback)(value);
                             }
                             popover->hide();
                         });
    }
    m_popover->setContentWidget(content);
    finalizeRoot();
}

void ScreenshotToolPaletteIconOptionEditor::update(int value, bool mixed) {
    const auto currentOption = std::find_if(
        m_options.cbegin(), m_options.cend(),
        [value](const ScreenshotToolPaletteIconOption& option) { return option.value == value; });
    if (m_trigger != nullptr) {
        if (currentOption != m_options.cend()) {
            m_trigger->setValueIconRef(currentOption->icon);
        }
        m_trigger->setMixed(mixed);
    }
    for (int index = 0; index < m_buttons.size() && index < m_options.size(); ++index) {
        setButtonActive(m_buttons.at(index), !mixed && m_options.at(index).value == value);
    }
}

void ScreenshotToolPaletteIconOptionEditor::rebind(
    const ScreenshotToolPaletteIconOptionEditorConfig& config,
    const std::function<void(int)>& setValue) {
    if (m_setValue != nullptr) {
        *m_setValue = setValue;
    }
    configureStylePopupTrigger(m_trigger, config.accessibleName);
    for (int index = 0; index < m_buttons.size() && index < config.options.size(); ++index) {
        configureScreenshotToolPaletteTooltip(m_buttons.at(index),
                                              config.options.at(index).tooltip);
    }
}

void ScreenshotToolPaletteIconOptionEditor::refreshMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    refreshRootMetrics(metrics);
    if (screenshotToolPaletteMetricsApplyTo(metrics, m_trigger)) {
        configureScreenshotToolPaletteIconValuePreviewTrigger(m_trigger, metrics);
    }
    for (adqt::widgets::AdButton* button : m_buttons) {
        resetPopupButtonControlScale(button);
    }
}

void ScreenshotToolPaletteIconOptionEditor::resetPopupMetrics(
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    static_cast<void>(metrics);
    for (adqt::widgets::AdButton* button : m_buttons) {
        resetPopupButtonControlScale(button);
    }
}

void ScreenshotToolPaletteIconOptionEditor::release() {
    m_trigger = nullptr;
    m_popover = nullptr;
    m_buttons.clear();
    m_options.clear();
    m_setValue.reset();
    releaseRoot();
}

ScreenshotToolPaletteNumericPresetEditorConfig
screenshotToolPaletteSizePresetEditorConfig(const QString& summaryTooltip,
                                            const QString& summaryObjectName,
                                            const char* presetTooltipPattern) {
    ScreenshotToolPaletteNumericPresetEditorConfig config;
    config.summaryTooltip = summaryTooltip;
    config.summaryObjectName = summaryObjectName;
    config.suffix = QStringLiteral("px");
    config.values = style_presets::sizePresetValues();
    config.presetTooltip = [presetTooltipPattern](int index, double value) {
        return style_presets::sizePresetTooltip(presetTooltipPattern, index, value);
    };
    config.presetIcon = [](int index) { return style_presets::sizePresetIcon(index); };
    return config;
}

const QStringList& screenshotToolPaletteFontFamilies() {
    // The palette evicts and rebuilds its font editors after every capture
    // reset and tool-family switch, so the system enumeration and normalization
    // run once per process instead of per editor build.
    static const QStringList cachedFamilies = [] {
        QStringList families;
        const QStringList systemFamilies = QFontDatabase::families();
        families.reserve(systemFamilies.size());
        for (const QString& family : systemFamilies) {
            const QString trimmed = family.trimmed();
            if (!trimmed.isEmpty()) {
                families.append(trimmed);
            }
        }
        families.removeDuplicates();
        families.sort(Qt::CaseInsensitive);
        return families;
    }();
    return cachedFamilies;
}

} // namespace snow_shot::presentation
