#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECONTROLS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECONTROLS_H

#include "screenshottoolpalettebuttons.h"
#include "screenshottoolpalettestylemodel.h"

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QColor>
#include <QPoint>
#include <QSet>
#include <optional>
#include <QVector>

#include <functional>

class QBoxLayout;
class QButtonGroup;
class QObject;
class QSpacerItem;
class QWidget;

namespace adqt::widgets {
class AdButton;
class AdColorPicker;
class AdLineEdit;
class AdPopover;
class AdSelect;
class AdSlider;
class AdRadio;
class AdRadioButtonGroup;
} // namespace adqt::widgets

struct ScreenshotToolPaletteStyleControlCallbacks {
    std::function<void(const SnowCanvasShapeStyle& style, quint32 properties,
                       SnowCanvasShapeKind kind)>
        shapeStyleChanged;
    std::function<void(const SnowCanvasTextStyle& style)> textStyleChanged;
    std::function<void()> textStylePopupInteractionBegan;
    std::function<void()> textStylePopupInteractionEnded;
    std::function<void(const SnowCanvasSerialNumberStyle& style)> serialNumberStyleChanged;
    std::function<void()> serialNumberDecrementRequested;
    std::function<void()> serialNumberIncrementRequested;
    std::function<void()> serialNumberCreateTextRequested;
    std::function<void(const SnowCanvasWatermarkConfig& config)> watermarkConfigChanged;
    std::function<void(const SnowCanvasWatermarkConfig& config)> watermarkPreviewChanged;
    std::function<void()> visibleContentChanged;
    std::function<void(adqt::widgets::AdColorPicker* picker)> canvasColorSamplingRequested;
};

class ScreenshotToolPaletteStyleControls final : private ScreenshotToolPaletteStyleState {
  public:
    explicit ScreenshotToolPaletteStyleControls(
        ScreenshotToolPaletteStyleControlCallbacks callbacks,
        const SnowCanvasStyleDefaults& defaults);

    [[nodiscard]] ScreenshotToolPaletteStyleState& styleState();
    [[nodiscard]] const ScreenshotToolPaletteStyleState& styleState() const;

    void addStrokeWidthControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                const ScreenshotToolPaletteButtonMetrics& metrics);
    void addStrokeColorControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                const ScreenshotToolPaletteButtonMetrics& metrics);
    void addFillColorControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                              const ScreenshotToolPaletteButtonMetrics& metrics);
    void addCornerRadiusControl(QBoxLayout* layout, QWidget* parent,
                                const ScreenshotToolPaletteButtonMetrics& metrics);
    void addArrowControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                          const std::function<void()>& addGroupSeparator,
                          const ScreenshotToolPaletteButtonMetrics& metrics);
    void addShapeControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                          const ScreenshotToolPaletteButtonMetrics& metrics);
    void addToolbarSpacing(QBoxLayout* layout, int baseSpacing,
                           const ScreenshotToolPaletteButtonMetrics& metrics);
    void addHighlightControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                              const std::function<void()>& addGroupSeparator,
                              const ScreenshotToolPaletteButtonMetrics& metrics);
    void addPenHighlightControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                 const std::function<void()>& addGroupSeparator,
                                 const ScreenshotToolPaletteButtonMetrics& metrics);
    void addTextControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                         const std::function<void()>& addGroupSeparator,
                         const ScreenshotToolPaletteButtonMetrics& metrics);
    void addWatermarkControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                              const std::function<void()>& addGroupSeparator,
                              const ScreenshotToolPaletteButtonMetrics& metrics);
    void addSerialNumberControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                 const std::function<void()>& addGroupSeparator,
                                 const ScreenshotToolPaletteButtonMetrics& metrics);

    void reset();
    // Drop widget/editor bindings while retaining the persistent style model.
    // The next materialization recreates controls from the retained values.
    void releaseControlBindings();
    [[nodiscard]] bool stepStrokeWidth(int direction);
    void setLineControlsActive(bool active);
    void setFreeDrawControlsActive(bool active);
    void setHighlightControlsActive(bool active);
    void setPenHighlightControlsActive(bool active);
    void setArrowControlsActive(bool active);
    void setTextControlsActive(bool active);
    void clearTextStylePopupInteractions();
    [[nodiscard]] bool stepTextFontSize(int direction);
    [[nodiscard]] bool handleCornerRadiusWheel(const QPoint& globalPosition, int direction);
    [[nodiscard]] bool handleTextStrokeWidthWheel(const QPoint& globalPosition, int direction);
    [[nodiscard]] bool handleTextCornerRadiusWheel(const QPoint& globalPosition, int direction);
    [[nodiscard]] bool handleSerialNumberWheel(const QPoint& globalPosition, int direction);
    [[nodiscard]] bool handleWatermarkWheel(const QPoint& globalPosition, int direction);
    [[nodiscard]] bool stepWatermarkFontSize(int direction);
    [[nodiscard]] SnowCanvasShapeStyle rectangleStyle() const;
    [[nodiscard]] SnowCanvasArrowStyle arrowStyle() const;
    [[nodiscard]] SnowCanvasTextStyle textStyle() const;
    [[nodiscard]] SnowCanvasStyleDefaults creationStyleDefaults() const;
    void setCreationStyleDefaults(const SnowCanvasStyleDefaults& defaults);
    void setRectangleStyle(const SnowCanvasShapeStyle& style);
    void setWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void setStyleToolbarState(const SnowCanvasStyleToolbarState& state);
    void setSerialNumberControlsVisible(bool visible);

    // Popup content owns its window DPR and is intentionally excluded.
    void refreshToolbarMetrics(const ScreenshotToolPaletteButtonMetrics& metrics);
    void addSpotlightColorControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                   const QColor& initialColor, const QVector<QColor>& presetValues,
                                   const std::function<void(const QColor&)>& commitColor,
                                   const std::function<void(const QColor&)>& previewColor,
                                   const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateSpotlightColorControls(const QColor& color);
    void addPenFilterStrokeWidthControls(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                         double initialWidth,
                                         const std::function<void()>& cycleWidth,
                                         const std::function<void(double)>& setWidth,
                                         const ScreenshotToolPaletteButtonMetrics& metrics);
    void updatePenFilterStrokeWidthControls(double width, bool mixed);
    void refreshThemeIcons(const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateRectangleStyleControls(quint32 groups = 0xffffffffu);
    void updateTextStyleControls(quint32 groups = 0xffffffffu);

#if defined(SNOW_SHOT_TEST_HOOKS)
    [[nodiscard]] quint64 styleStateNoopCount() const;
    [[nodiscard]] quint64 propertyGroupRefreshCount() const;
#endif

  private:
    void setStrokeWidth(double strokeWidth);
    void cycleStrokeWidth();
    void setStrokeColor(const QColor& color);
    void setStrokeStyle(SnowCanvasStrokeStyle strokeStyle);
    void setFillColor(const QColor& color);
    void setFillStyle(SnowCanvasFillStyle fillStyle);
    void setCornerRadius(int cornerRadius);
    void setShape(SnowCanvasRectangleShape shape);
    void setPenHighlightColor(const QColor& color);
    void setPenHighlightStrokeWidth(double strokeWidth);
    void setArrowStrokeWidth(double strokeWidth);
    void cycleArrowStrokeWidth();
    void setArrowStrokeColor(const QColor& color);
    void setArrowStrokeStyle(SnowCanvasStrokeStyle strokeStyle);
    void setArrowType(SnowCanvasArrowType arrowType);
    void setArrowhead(bool start, SnowCanvasArrowhead arrowhead);
    void setTextColor(const QColor& color);
    void setTextFontSize(double fontSize);
    void cycleTextFontSize();
    void setTextFontFamily(const QString& fontFamily);
    void setTextStrokeColor(const QColor& color);
    void setTextStrokeWidth(double strokeWidth);
    void setTextFillColor(const QColor& color);
    void setTextFillStyle(SnowCanvasFillStyle fillStyle);
    void setTextCornerRadius(int cornerRadius);
    void setTextHorizontalAlign(SnowCanvasTextHorizontalAlign alignment);
    void setWatermarkColor(const QColor& color);
    void setWatermarkFontSize(double fontSize);
    void cycleWatermarkFontSize();
    void setWatermarkFontFamily(const QString& fontFamily);
    void setWatermarkAngle(double angle);
    void setWatermarkGap(double gap);
    void setWatermarkOpacity(double opacity);
    void setSerialNumberColor(const QColor& color);
    void setSerialNumberFillColor(const QColor& color);
    void setSerialNumberFillStyle(SnowCanvasFillStyle fillStyle);
    void setSerialNumber(qint64 number);
    void setSerialNumberFontSize(double fontSize);
    void cycleSerialNumberFontSize();
    void setSerialNumberFontFamily(const QString& fontFamily);
    [[nodiscard]] bool hasMixedProperty(quint32 property) const;
    void clearMixedProperties(quint32 properties);
    void notifyShapeStyleChanged(const SnowCanvasShapeStyle& style, quint32 properties,
                                 SnowCanvasShapeKind kind) const;
    void updateArrowStyleControls(quint32 groups = 0xffffffffu);
    void updateHighlightStyleControls(quint32 groups = 0xffffffffu);
    void updatePenHighlightStyleControls(quint32 groups = 0xffffffffu);
    void updateRectangleOnlyControlsVisibility();
    [[nodiscard]] ScreenshotToolPaletteRectangleStyleModel& activeShapeStyle();
    [[nodiscard]] const ScreenshotToolPaletteRectangleStyleModel& activeShapeStyle() const;
    [[nodiscard]] ScreenshotToolPaletteRectangleStyleModel& activeCreationShapeStyle();
    [[nodiscard]] SnowCanvasShapeKind activeShapeKind() const;
    void notifyTextStyleChanged() const;
    void updateWatermarkControls();
    void refreshWatermarkOpacityMetrics(const ScreenshotToolPaletteButtonMetrics& metrics);
    void notifyWatermarkConfigChanged() const;
    void notifyWatermarkPreviewChanged() const;
    void updateSerialNumberStyleControls(quint32 groups = 0xffffffffu);
    void notifySerialNumberStyleChanged() const;
    void observeTextStylePopup(adqt::widgets::AdColorPicker* picker);
    void observeTextStylePopup(adqt::widgets::AdSelect* select);
    void beginTextStylePopupInteraction(QObject* popup);
    void endTextStylePopupInteraction(QObject* popup);

    struct FontEditor {
        NumericValuePreviewButton* sizeSummary = nullptr;
        QVector<adqt::widgets::AdButton*> sizePresets;
        QVector<double> sizeValues;
        adqt::widgets::AdSelect* familySelect = nullptr;
    };

    struct FontEditorConfig {
        QString accessibleName;
        QString summaryTooltip;
        QString summaryObjectName;
        QVector<double> sizeValues;
        std::function<ScreenshotToolPaletteTranslationText(int index, double value)> presetTooltip;
        bool observePopup = false;
    };

    [[nodiscard]] FontEditor addFontEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                           const FontEditorConfig& config, double initialSize,
                                           const QString& initialFamily,
                                           const std::function<void()>& cycleSize,
                                           const std::function<void(double)>& setSize,
                                           const std::function<void(const QString&)>& setFamily,
                                           const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateFontEditor(FontEditor& editor, double size, const QString& family, bool sizeMixed,
                          bool familyMixed, quint32 groups, quint32 sizeGroup, quint32 familyGroup);
    void refreshFontEditorMetrics(FontEditor& editor,
                                  const ScreenshotToolPaletteButtonMetrics& metrics);
    adqt::widgets::AdColorPicker* createColorPickerShell(QWidget* parent,
                                                         const QString& accessibleName,
                                                         const QColor& initialColor,
                                                         bool alphaEnabled, bool observePopup);
    void connectColorPickerChanges(adqt::widgets::AdColorPicker* picker, QObject* receiver,
                                   bool* handlingChange,
                                   const std::function<void(const QColor&)>& commitColor,
                                   const std::function<void(const QColor&)>& previewColor = {},
                                   const std::function<void(const QColor&)>& valueChanged = {});

    struct ColorPickerPopupLayout {
        QWidget* widget = nullptr;
        QBoxLayout* layout = nullptr;
    };

    [[nodiscard]] ColorPickerPopupLayout
    createColorPickerPopupContent(adqt::widgets::AdColorPicker* picker, const QString& objectName,
                                  const ScreenshotToolPaletteButtonMetrics& popupMetrics);
    [[nodiscard]] ColorPickerPopupLayout
    addColorPickerPopupRow(ColorPickerPopupLayout& content, const QString& objectName,
                           const ScreenshotToolPaletteButtonMetrics& popupMetrics);

    struct IconOption {
        int value = 0;
        ScreenshotToolPaletteTranslationText tooltip;
        adqt::icons::IconRef icon;
    };

    struct IconOptionEditor {
        IconValuePreviewTrigger* trigger = nullptr;
        adqt::widgets::AdPopover* popover = nullptr;
        QVector<adqt::widgets::AdButton*> buttons;
        QVector<IconOption> options;
    };

    struct IconOptionEditorConfig {
        QString accessibleName;
        QString triggerTooltip;
        QVector<IconOption> options;
        int gridColumnCount = 0;
        bool minimizeTitleWidth = false;
    };

    [[nodiscard]] IconOptionEditor
    addIconOptionEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                        const IconOptionEditorConfig& config, int initialValue,
                        const std::function<void(int)>& setValue,
                        const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateIconOptionEditor(IconOptionEditor& editor, int value, bool mixed);
    void refreshIconOptionEditorMetrics(IconOptionEditor& editor,
                                        const ScreenshotToolPaletteButtonMetrics& metrics);

    struct ColorEditor {
        adqt::widgets::AdColorPicker* picker = nullptr;
        ColorSwatchButton* trigger = nullptr;
        QVector<adqt::widgets::AdButton*> presets;
        QVector<QColor> presetValues;
    };

    struct ColorEditorConfig {
        QString accessibleName;
        QString pickerObjectName;
        QString triggerObjectName;
        QVector<QColor> presetValues;
        std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> presetTooltip;
        bool alphaEnabled = false;
        bool observePopup = false;
    };

    [[nodiscard]] ColorEditor addColorEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                             const ColorEditorConfig& config,
                                             const QColor& initialColor, bool* handlingChange,
                                             const std::function<void(const QColor&)>& commitColor,
                                             const std::function<void(const QColor&)>& previewColor,
                                             const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateColorEditor(ColorEditor& editor, const QColor& color, bool mixed);
    void refreshColorEditorMetrics(ColorEditor& editor,
                                   const ScreenshotToolPaletteButtonMetrics& metrics);

    struct FillEditor {
        adqt::widgets::AdColorPicker* picker = nullptr;
        FillStylePreviewTrigger* trigger = nullptr;
        QVector<adqt::widgets::AdButton*> colorPresets;
        QVector<QColor> colorValues;
        QVector<FillStylePreviewButton*> styleButtons;
        QVector<SnowCanvasFillStyle> styleValues;
    };

    struct FillEditorConfig {
        QString accessibleName;
        QString popupObjectName;
        QString presetRowObjectName;
        QVector<QColor> colorValues;
        std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> colorTooltip;
        std::function<ScreenshotToolPaletteTranslationText(SnowCanvasFillStyle style)> styleTooltip;
        bool observePopup = false;
    };

    [[nodiscard]] FillEditor addFillEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                                           const FillEditorConfig& config,
                                           const QColor& initialColor,
                                           SnowCanvasFillStyle initialStyle, bool* handlingChange,
                                           const std::function<void(const QColor&)>& setColor,
                                           const std::function<void(SnowCanvasFillStyle)>& setStyle,
                                           const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateFillEditor(FillEditor& editor, const QColor& color, SnowCanvasFillStyle style,
                          bool colorMixed, bool styleMixed);
    void refreshFillEditorMetrics(FillEditor& editor,
                                  const ScreenshotToolPaletteButtonMetrics& metrics);

    struct StrokeEditor {
        adqt::widgets::AdColorPicker* picker = nullptr;
        StrokeStylePreviewTrigger* trigger = nullptr;
        QVector<adqt::widgets::AdButton*> colorPresets;
        QVector<QColor> colorValues;
        QVector<StrokeStylePreviewButton*> styleButtons;
        QVector<SnowCanvasStrokeStyle> styleValues;
    };

    struct StrokeEditorConfig {
        QString accessibleName;
        QString popupObjectName;
        QString styleRowObjectName;
        QVector<QColor> colorValues;
        std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> colorTooltip;
        std::function<ScreenshotToolPaletteTranslationText(SnowCanvasStrokeStyle style)>
            styleTooltip;
    };

    [[nodiscard]] StrokeEditor
    addStrokeEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                    const StrokeEditorConfig& config, const QColor& initialColor,
                    SnowCanvasStrokeStyle initialStyle, bool* handlingChange,
                    const std::function<void(const QColor&)>& setColor,
                    const std::function<void(SnowCanvasStrokeStyle)>& setStyle,
                    const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateStrokeEditor(StrokeEditor& editor, const QColor& color,
                            SnowCanvasStrokeStyle style, bool colorMixed, bool styleMixed);
    void refreshStrokeEditorMetrics(StrokeEditor& editor,
                                    const ScreenshotToolPaletteButtonMetrics& metrics);

    struct NumericPresetEditor {
        adqt::widgets::AdButton* summary = nullptr;
        QVector<adqt::widgets::AdButton*> presets;
        QVector<double> values;
        bool strokePreview = false;
    };

    struct NumericPresetEditorConfig {
        QString summaryTooltip;
        QString summaryObjectName;
        QString suffix;
        QVector<double> values;
        std::function<ScreenshotToolPaletteTranslationText(int index, double value)> presetTooltip;
        std::function<QString(double value)> presetObjectName;
        std::function<adqt::icons::IconRef(int index)> presetIcon;
        bool strokePreview = false;
    };

    [[nodiscard]] NumericPresetEditor
    addNumericPresetEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                           const NumericPresetEditorConfig& config, double initialValue,
                           const std::function<void()>& cycleValue,
                           const std::function<void(double)>& setValue,
                           const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateNumericPresetEditor(NumericPresetEditor& editor, double value, bool mixed);
    void refreshNumericPresetEditorMetrics(NumericPresetEditor& editor,
                                           const ScreenshotToolPaletteButtonMetrics& metrics);

    struct WidthColorEditor {
        adqt::widgets::AdColorPicker* picker = nullptr;
        StrokeWidthPreviewButton* trigger = nullptr;
        QVector<adqt::widgets::AdButton*> widthButtons;
        QVector<double> widthValues;
        QVector<adqt::widgets::AdButton*> colorButtons;
        QVector<QColor> colorValues;
    };

    struct WidthColorEditorConfig {
        QString accessibleName;
        QString triggerTooltip;
        QString popupObjectName;
        QString widthRowObjectName;
        QString colorRowObjectName;
        QVector<double> widthValues;
        QVector<QColor> colorValues;
        std::function<ScreenshotToolPaletteTranslationText(double value)> widthTooltip;
        std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> colorTooltip;
        bool observePopup = false;
    };

    [[nodiscard]] WidthColorEditor
    addWidthColorEditor(QBoxLayout* layout, QWidget* parent, QObject* receiver,
                        const WidthColorEditorConfig& config, double initialWidth,
                        const QColor& initialColor, bool* handlingChange,
                        const std::function<void(double)>& setWidth,
                        const std::function<void(const QColor&)>& setColor,
                        const ScreenshotToolPaletteButtonMetrics& metrics);
    void updateWidthColorEditor(WidthColorEditor& editor, double width, const QColor& color,
                                bool widthMixed, bool colorMixed);
    void refreshWidthColorEditorMetrics(WidthColorEditor& editor,
                                        const ScreenshotToolPaletteButtonMetrics& metrics);

    enum ShapeRefreshGroup : quint32 {
        ShapeModeRefresh = 1u << 0,
        ShapeStrokeWidthRefresh = 1u << 1,
        ShapeStrokeRefresh = 1u << 2,
        ShapeFillRefresh = 1u << 3,
        ShapeCornerRefresh = 1u << 4,
        ShapeArrowTypeRefresh = 1u << 5,
        ShapeArrowheadsRefresh = 1u << 6,
        AllShapeRefreshes = (1u << 7) - 1,
    };
    enum TextRefreshGroup : quint32 {
        TextColorRefresh = 1u << 0,
        TextFontSizeRefresh = 1u << 1,
        TextFontFamilyRefresh = 1u << 2,
        TextStrokeRefresh = 1u << 3,
        TextFillRefresh = 1u << 4,
        TextCornerRefresh = 1u << 5,
        TextAlignmentRefresh = 1u << 6,
        AllTextRefreshes = (1u << 7) - 1,
    };
    enum SerialNumberRefreshGroup : quint32 {
        SerialNumberValueRefresh = 1u << 0,
        SerialNumberColorRefresh = 1u << 1,
        SerialNumberFillRefresh = 1u << 2,
        SerialNumberFontSizeRefresh = 1u << 3,
        SerialNumberFontFamilyRefresh = 1u << 4,
        AllSerialNumberRefreshes = (1u << 5) - 1,
    };

    bool m_handlingStrokeColorPickerChange = false;
    bool m_handlingArrowStrokeColorPickerChange = false;
    bool m_handlingTextColorPickerChange = false;
    bool m_handlingTextStrokeColorPickerChange = false;
    bool m_handlingTextFillColorPickerChange = false;
    bool m_handlingHighlightColorPickerChange = false;
    bool m_handlingHighlightStrokeColorPickerChange = false;
    bool m_handlingPenHighlightColorPickerChange = false;
    bool m_handlingSerialNumberColorPickerChange = false;
    bool m_handlingSerialNumberFillColorPickerChange = false;
    QSet<QObject*> m_openTextStylePopups;
    ScreenshotToolPaletteStyleControlCallbacks m_callbacks;
    const SnowCanvasStyleDefaults m_defaults;
    NumericPresetEditor m_shapeStrokeWidthEditor;
    StrokeEditor m_shapeStrokeEditor;
    FillEditor m_shapeFillEditor;
    CornerRadiusEditorButton* m_cornerRadiusEditor = nullptr;
    QWidget* m_shapeControlsContainer = nullptr;
    ColorEditor m_highlightColorEditor;
    ColorEditor m_spotlightColorEditor;
    adqt::widgets::AdRadioButtonGroup* m_shapeButtonGroup = nullptr;
    adqt::widgets::AdRadio* m_rectangleShapeButton = nullptr;
    adqt::widgets::AdRadio* m_ellipseShapeButton = nullptr;
    adqt::widgets::AdRadio* m_diamondShapeButton = nullptr;
    WidthColorEditor m_highlightStrokeEditor;
    ColorEditor m_penHighlightColorEditor;
    NumericPresetEditor m_penHighlightStrokeWidthEditor;
    NumericPresetEditor m_penFilterStrokeWidthEditor;
    NumericPresetEditor m_arrowStrokeWidthEditor;
    StrokeEditor m_arrowStrokeEditor;
    QWidget* m_arrowTypeControlsContainer = nullptr;
    adqt::widgets::AdRadioButtonGroup* m_arrowTypeButtonGroup = nullptr;
    IconOptionEditor m_startArrowheadEditor;
    IconOptionEditor m_endArrowheadEditor;
    ColorEditor m_textColorEditor;
    FontEditor m_textFontEditor;
    WidthColorEditor m_textStrokeEditor;
    FillEditor m_textFillEditor;
    CornerRadiusEditorButton* m_textCornerRadiusEditor = nullptr;
    IconOptionEditor m_textAlignmentEditor;
    ColorEditor m_serialNumberColorEditor;
    FillEditor m_serialNumberFillEditor;
    CornerRadiusEditorButton* m_serialNumberEditor = nullptr;
    FontEditor m_serialNumberFontEditor;
    bool m_watermarkColorPreviewPending = false;
    ColorEditor m_watermarkColorEditor;
    adqt::widgets::AdLineEdit* m_watermarkTextEdit = nullptr;
    FontEditor m_watermarkFontEditor;
    IconNumericValuePreviewButton* m_watermarkAngleEditor = nullptr;
    IconNumericValuePreviewButton* m_watermarkGapEditor = nullptr;
    ScreenshotToolPaletteSliderEditor m_watermarkOpacityEditor;

    struct ToolbarSpacingItem {
        QSpacerItem* item = nullptr;
        QWidget* owner = nullptr;
        int baseSpacing = 0;
    };
    QVector<ToolbarSpacingItem> m_toolbarSpacingItems;
#if defined(SNOW_SHOT_TEST_HOOKS)
    quint64 m_styleStateNoopCount = 0;
    quint64 m_propertyGroupRefreshCount = 0;
#endif
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECONTROLS_H
