#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECONTROLS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECONTROLS_H

#include "screenshottoolpalettestylecomponents.h"
#include "screenshottoolpalettestylemodel.h"

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QColor>
#include <QByteArray>
#include <QPoint>
#include <QSet>
#include <QVector>

#include <functional>
#include <memory>
#include <vector>

class QBoxLayout;
class QFrame;
class QLabel;
class QObject;
class QSpacerItem;
class QWidget;

namespace adqt::widgets {
class AdLineEdit;
class AdSelect;
class AdSlider;
class AdRadioButtonGroup;
} // namespace adqt::widgets

// The shared editor components live in snow_shot::presentation; surface them
// unqualified for this palette-internal header.
using ScreenshotToolPaletteStyleEditorComponent =
    snow_shot::presentation::ScreenshotToolPaletteStyleEditorComponent;
using ScreenshotToolPaletteEditorServices =
    snow_shot::presentation::ScreenshotToolPaletteEditorServices;
using ScreenshotToolPaletteColorEditor = snow_shot::presentation::ScreenshotToolPaletteColorEditor;
using ScreenshotToolPaletteStrokeEditor =
    snow_shot::presentation::ScreenshotToolPaletteStrokeEditor;
using ScreenshotToolPaletteFillEditor = snow_shot::presentation::ScreenshotToolPaletteFillEditor;
using ScreenshotToolPaletteWidthColorEditor =
    snow_shot::presentation::ScreenshotToolPaletteWidthColorEditor;
using ScreenshotToolPaletteNumericPresetEditor =
    snow_shot::presentation::ScreenshotToolPaletteNumericPresetEditor;
using ScreenshotToolPaletteFontEditor = snow_shot::presentation::ScreenshotToolPaletteFontEditor;
using ScreenshotToolPaletteIconOptionEditor =
    snow_shot::presentation::ScreenshotToolPaletteIconOptionEditor;
using ScreenshotToolPaletteColorEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteColorEditorConfig;
using ScreenshotToolPaletteStrokeEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteStrokeEditorConfig;
using ScreenshotToolPaletteFillEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteFillEditorConfig;
using ScreenshotToolPaletteWidthColorEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteWidthColorEditorConfig;
using ScreenshotToolPaletteNumericPresetEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteNumericPresetEditorConfig;
using ScreenshotToolPaletteFontEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteFontEditorConfig;
using ScreenshotToolPaletteIconOptionEditorConfig =
    snow_shot::presentation::ScreenshotToolPaletteIconOptionEditorConfig;

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

// Palette-owned row services used by the family builders. The palette supplies
// separator/spacing/mode-selector creation (it tracks those widgets for
// layout profiling and visibility toggling) so the style-control layer stays
// free of palette internals.
struct ScreenshotToolPaletteStyleModeSelectorOption {
    int id = 0;
    QString tooltip;
    adqt::icons::IconRef icon;
};

struct ScreenshotToolPaletteStyleFamilyHost {
    // Registers a family row layout for palette-wide spacing synchronization.
    std::function<void(QBoxLayout* layout)> registerRowLayout;
    // Group spacing + separator + group spacing.
    std::function<void(QBoxLayout* layout)> addGroupSeparator;
    // Single item spacing inside a row.
    std::function<void(QBoxLayout* layout)> addItemSpacing;
    // Single group-spacing spacer (flanks of the named shape separator).
    std::function<QSpacerItem*(QBoxLayout* layout)> addGroupSpacing;
    // Group-spacing spacer inserted at a layout index (mode-selector rows).
    std::function<void(QBoxLayout* layout, int index)> insertGroupSpacing;
    std::function<QFrame*(QWidget* parent, const QString& objectName)> createSeparator;
    std::function<QWidget*(QWidget* parent, const QString& objectName, int initialId,
                           const QVector<ScreenshotToolPaletteStyleModeSelectorOption>& options)>
        createModeSelector;
    // Scaled STYLE_ITEM_SPACING used as the initial row layout spacing.
    int rowItemSpacing = 0;
};

struct ScreenshotToolPaletteShapeFamilyResult {
    QWidget* controls = nullptr;
    QFrame* shapeGroupSeparator = nullptr;
    QSpacerItem* shapeGroupSeparatorLeadingSpacing = nullptr;
    QSpacerItem* shapeGroupSeparatorTrailingSpacing = nullptr;
};

struct ScreenshotToolPaletteHighlightFamilyResult {
    QWidget* rectangleControls = nullptr;
    QWidget* penControls = nullptr;
};

// Behavior hooks for the spotlight row; the palette keeps spotlight config
// emission and replay suppression.
struct ScreenshotToolPaletteSpotlightCallbacks {
    std::function<void(const QColor& color)> commitColor;
    std::function<void(const QColor& color)> previewColor;
    std::function<void(double opacity)> setOpacity;
};

struct ScreenshotToolPaletteFilterFamilyResult {
    QWidget* controls = nullptr;
    adqt::widgets::AdSelect* typeSelect = nullptr;
    QLabel* intensityIcon = nullptr;
    adqt::widgets::AdSlider* intensitySlider = nullptr;
};

// Behavior hooks for a filter row; the palette owns filter state mutations,
// intensity availability and the filterStyleChanged signal.
struct ScreenshotToolPaletteFilterCallbacks {
    std::function<void(int typeValue)> setType;
    std::function<void(double strength)> setStrength;
    std::function<void()> cycleStrokeWidth;
    std::function<void(double strokeWidth)> setStrokeWidth;
};

struct ScreenshotToolPaletteFilterFamilyConfig {
    QString controlsObjectName;
    QString typeSelectObjectName;
    QString intensityIconObjectName;
    QString intensitySliderObjectName;
    bool includeStrokeWidth = false;
    double initialStrokeWidth = 0.0;
};

struct ScreenshotToolPaletteStyleReconcileStats {
    int retained = 0;
    int created = 0;
    int destroyed = 0;
};

// Owns the drawing style state and builds every sub-toolbar style editor row
// from the shared editor components. The palette drives family materialization
// (buildXxxFamily) and tool activation (setXxxControlsActive); inbound canvas
// state flows in through setStyleToolbarState.
class ScreenshotToolPaletteStyleControls final {
  public:
    explicit ScreenshotToolPaletteStyleControls(
        ScreenshotToolPaletteStyleControlCallbacks callbacks,
        const SnowCanvasStyleDefaults& defaults);

    [[nodiscard]] ScreenshotToolPaletteStyleState& styleState();
    [[nodiscard]] const ScreenshotToolPaletteStyleState& styleState() const;

    // Stages compatible editor roots from the visible source composition so
    // the destination builder can adopt them without rebuilding their widget
    // and popup subtrees.
    void parkStyleEditors(int tool, QWidget* controls);
    void restoreStyleEditors(int tool, QWidget* controls);
    void prepareStyleReconcile(int sourceTool, int destinationTool, QWidget* sourceControls);
    void stageDestinationStyleEditors(int destinationTool, QWidget* destinationControls);
    void stageExternalStyleEditorWidget(const char* role, const char* signature, QWidget* widget);
    void finishStyleReconcile(int destinationTool);
    void discardBindingsExcept(int destinationTool, QWidget* destinationControls);
    [[nodiscard]] ScreenshotToolPaletteStyleReconcileStats lastReconcileStats() const;

    // Style family row builders. Each creates the family row widget (parented
    // to the palette style panel), wires the shared editor components to the
    // style state and registers them for state-driven refreshes.
    [[nodiscard]] ScreenshotToolPaletteShapeFamilyResult
    buildShapeFamily(int tool, QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
                     const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] QWidget* buildArrowFamily(QWidget* panel,
                                            const ScreenshotToolPaletteStyleFamilyHost& host,
                                            const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] ScreenshotToolPaletteHighlightFamilyResult
    buildHighlightFamily(int tool, QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
                         const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] QWidget*
    buildSpotlightFamily(QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
                         const ScreenshotToolPaletteSpotlightCallbacks& spotlightCallbacks,
                         const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] QWidget* buildTextFamily(QWidget* panel,
                                           const ScreenshotToolPaletteStyleFamilyHost& host,
                                           const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] QWidget*
    buildSerialNumberFamily(QWidget* panel, const ScreenshotToolPaletteStyleFamilyHost& host,
                            const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] QWidget* buildWatermarkFamily(QWidget* panel,
                                                const ScreenshotToolPaletteStyleFamilyHost& host,
                                                const ScreenshotToolPaletteButtonMetrics& metrics);
    [[nodiscard]] ScreenshotToolPaletteFilterFamilyResult
    buildFilterFamily(const ScreenshotToolPaletteFilterFamilyConfig& config,
                      const ScreenshotToolPaletteFilterCallbacks& callbacks, QWidget* panel,
                      const ScreenshotToolPaletteStyleFamilyHost& host,
                      const ScreenshotToolPaletteButtonMetrics& metrics);

    void reset();
    // Drop component/widget bindings while retaining the persistent style
    // model. The next materialization recreates controls from the retained
    // values.
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
    [[nodiscard]] SnowCanvasStyleDefaults creationStyleDefaults() const;
    void setCreationStyleDefaults(const SnowCanvasStyleDefaults& defaults);
    void setRectangleStyle(const SnowCanvasShapeStyle& style);
    void setWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void setStyleToolbarState(const SnowCanvasStyleToolbarState& state);
    void setSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    void updateSpotlightColorControls(const QColor& color);
    [[nodiscard]] adqt::widgets::AdSlider* spotlightOpacitySlider() const;
    [[nodiscard]] QLabel* spotlightOpacityIcon() const;
    void updatePenFilterStrokeWidthControls(double width, bool mixed);
    [[nodiscard]] int spacerReferenceWidth(const QSpacerItem* spacer) const;

    // Popup content owns its window DPR and is intentionally excluded.
    void refreshToolbarMetrics(const ScreenshotToolPaletteButtonMetrics& metrics);
    void refreshThemeIcons(const ScreenshotToolPaletteButtonMetrics& metrics);

#if defined(SNOW_SHOT_TEST_HOOKS)
    [[nodiscard]] quint64 styleStateNoopCount() const;
    [[nodiscard]] quint64 propertyGroupRefreshCount() const;
#endif

  private:
    // Refresh-group bits mirror the SnowCanvasStyleToolbarState property
    // masks per style kind; every editor registers the bits that drive it.
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
    static constexpr quint32 kAllRefreshGroups = 0xffffffffu;

    // One refreshable editor: the refresh-group bits it reacts to and the
    // applyState closure that pushes model values and mixed flags into it.
    // Metrics/theme refresh is driven separately through the registered
    // component list.
    struct StyleEditorEntry {
        quint32 groupMask = 0;
        std::function<void()> applyState;
    };

    void refreshEditorEntries(QVector<StyleEditorEntry>& entries, quint32 groups,
                              quint32 allGroups);
    void applyEditorEntries(const QVector<StyleEditorEntry>& entries, quint32 groups);
    void registerEditor(ScreenshotToolPaletteStyleEditorComponent* component);
    [[nodiscard]] QWidget* createRowWidget(QWidget* panel, const QString& objectName,
                                           const ScreenshotToolPaletteStyleFamilyHost& host) const;
    [[nodiscard]] ScreenshotToolPaletteEditorServices editorServices();
    void registerShapeEntries();
    void registerHighlightEntries();
    void registerPenHighlightEntries();
    void registerArrowEntries();
    void registerTextEntries();
    void registerSerialNumberEntries();
    void registerWatermarkEntries();

    // Generic property commits: mutate the owning model, mirror the creation
    // style, clear the mixed flag, refresh the family and notify.
    template <typename Apply, typename Mirror>
    void commitShapeProperty(quint32 property, Apply apply, Mirror mirror);
    template <typename Apply> void commitArrowProperty(quint32 property, Apply apply);
    template <typename Apply, typename Mirror>
    void commitPenHighlightProperty(quint32 property, Apply apply, Mirror mirror);
    template <typename Apply, typename Mirror>
    void commitTextProperty(quint32 mixedFlag, Apply apply, Mirror mirror);
    template <typename Apply> void commitSerialNumberProperty(quint32 mixedFlag, Apply apply);
    template <typename Apply> void commitWatermarkField(Apply apply);

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
    void updateRectangleStyleControls(quint32 groups = 0xffffffffu);
    void updateTextStyleControls(quint32 groups = 0xffffffffu);
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
    void refreshSpotlightOpacityMetrics(const ScreenshotToolPaletteButtonMetrics& metrics);
    void notifyWatermarkConfigChanged() const;
    void notifyWatermarkPreviewChanged() const;
    void updateSerialNumberStyleControls(quint32 groups = 0xffffffffu);
    void notifySerialNumberStyleChanged() const;
    void beginTextStylePopupInteraction(QObject* popup);
    void endTextStylePopupInteraction(QObject* popup);
    void addToolbarSpacing(QBoxLayout* layout, int baseSpacing,
                           const ScreenshotToolPaletteButtonMetrics& metrics);
    void tagEditor(ScreenshotToolPaletteStyleEditorComponent* component, const char* role,
                   const char* signature);
    [[nodiscard]] std::unique_ptr<ScreenshotToolPaletteStyleEditorComponent>
    takeReusableEditor(const char* role, const char* signature, QBoxLayout* destinationLayout,
                       QWidget* destinationParent);
    [[nodiscard]] QWidget* takeReusableWidget(const char* role, const char* signature,
                                              QBoxLayout* destinationLayout,
                                              QWidget* destinationParent);
    void stageReusableEditor(const char* role, const char* signature,
                             std::unique_ptr<ScreenshotToolPaletteStyleEditorComponent> editor);
    void stageReusableWidget(const char* role, const char* signature, QWidget* widget);
    [[nodiscard]] bool reusableRoleStaged(const char* role) const;
    [[nodiscard]] QWidget* editorRootForRole(QWidget* controls, const char* role) const;
    void rebuildRegisteredComponents();

    ScreenshotToolPaletteStyleState m_state;
    ScreenshotToolPaletteStyleControlCallbacks m_callbacks;
    const SnowCanvasStyleDefaults m_defaults;
    QSet<QObject*> m_openTextStylePopups;

    struct ReusableEditor {
        QByteArray role;
        QByteArray signature;
        std::unique_ptr<ScreenshotToolPaletteStyleEditorComponent> component;
        QWidget* widget = nullptr;
    };
    std::vector<ReusableEditor> m_reusableEditors;
    struct ParkedEditor {
        int tool = -1;
        QByteArray role;
        QByteArray signature;
        std::unique_ptr<ScreenshotToolPaletteStyleEditorComponent> component;
        QWidget* widget = nullptr;
    };
    std::vector<ParkedEditor> m_parkedEditors;
    ScreenshotToolPaletteStyleReconcileStats m_lastReconcileStats;
    int m_reconcileSourceTool = -1;
    int m_reconcileDestinationTool = -1;

    // Shared editor components. Owned here, refreshed through the registry
    // entries below; widget lifetimes belong to the family row widgets.
    std::unique_ptr<ScreenshotToolPaletteNumericPresetEditor> m_shapeStrokeWidthEditor;
    std::unique_ptr<ScreenshotToolPaletteStrokeEditor> m_shapeStrokeEditor;
    std::unique_ptr<ScreenshotToolPaletteFillEditor> m_shapeFillEditor;
    CornerRadiusEditorButton* m_cornerRadiusEditor = nullptr;
    QWidget* m_shapeControlsContainer = nullptr;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> m_highlightColorEditor;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> m_spotlightColorEditor;
    adqt::widgets::AdRadioButtonGroup* m_shapeButtonGroup = nullptr;
    std::unique_ptr<ScreenshotToolPaletteWidthColorEditor> m_highlightStrokeEditor;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> m_penHighlightColorEditor;
    std::unique_ptr<ScreenshotToolPaletteNumericPresetEditor> m_penHighlightStrokeWidthEditor;
    std::unique_ptr<ScreenshotToolPaletteNumericPresetEditor> m_penFilterStrokeWidthEditor;
    std::unique_ptr<ScreenshotToolPaletteNumericPresetEditor> m_arrowStrokeWidthEditor;
    std::unique_ptr<ScreenshotToolPaletteStrokeEditor> m_arrowStrokeEditor;
    adqt::widgets::AdRadioButtonGroup* m_arrowTypeButtonGroup = nullptr;
    std::unique_ptr<ScreenshotToolPaletteIconOptionEditor> m_startArrowheadEditor;
    std::unique_ptr<ScreenshotToolPaletteIconOptionEditor> m_endArrowheadEditor;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> m_textColorEditor;
    std::unique_ptr<ScreenshotToolPaletteFontEditor> m_textFontEditor;
    std::unique_ptr<ScreenshotToolPaletteWidthColorEditor> m_textStrokeEditor;
    std::unique_ptr<ScreenshotToolPaletteFillEditor> m_textFillEditor;
    CornerRadiusEditorButton* m_textCornerRadiusEditor = nullptr;
    std::unique_ptr<ScreenshotToolPaletteIconOptionEditor> m_textAlignmentEditor;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> m_serialNumberColorEditor;
    std::unique_ptr<ScreenshotToolPaletteFillEditor> m_serialNumberFillEditor;
    CornerRadiusEditorButton* m_serialNumberEditor = nullptr;
    std::unique_ptr<ScreenshotToolPaletteFontEditor> m_serialNumberFontEditor;
    bool m_watermarkColorPreviewPending = false;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> m_watermarkColorEditor;
    adqt::widgets::AdLineEdit* m_watermarkTextEdit = nullptr;
    std::unique_ptr<ScreenshotToolPaletteFontEditor> m_watermarkFontEditor;
    IconNumericValuePreviewButton* m_watermarkAngleEditor = nullptr;
    IconNumericValuePreviewButton* m_watermarkGapEditor = nullptr;
    ScreenshotToolPaletteSliderEditor m_watermarkOpacityEditor;
    ScreenshotToolPaletteSliderEditor m_spotlightOpacityEditor;

    QVector<ScreenshotToolPaletteStyleEditorComponent*> m_registeredComponents;
    QVector<StyleEditorEntry> m_shapeEntries;
    QVector<StyleEditorEntry> m_highlightEntries;
    QVector<StyleEditorEntry> m_penHighlightEntries;
    QVector<StyleEditorEntry> m_arrowEntries;
    QVector<StyleEditorEntry> m_textEntries;
    QVector<StyleEditorEntry> m_serialNumberEntries;
    QVector<StyleEditorEntry> m_watermarkEntries;

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
